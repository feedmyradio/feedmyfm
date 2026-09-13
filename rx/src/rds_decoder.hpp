// One FM station's RDS (Radio Data System) decoder.
//
//   raw discriminator output d[n] @ channel_rate (real, PRE de-emphasis --
//   this is MonoStation::m_disc, the full FM composite: audio + 19 kHz
//   pilot + 23..53 kHz L-R + 57 kHz RDS)
//     -> 57 kHz complex band-pass            (analytic RDS DSB-SC)
//     -> 19 kHz pilot band-pass -> unit-normalise -> CUBE  = a phase-locked
//        57 kHz reference.  stereo_decoder.hpp's `stereo.pilot_pll: false`
//        fallback squares the unit pilot the same way for a coherent 38 kHz
//        carrier (the pilot_pll:true default phase-doubles a PLL lock
//        instead); RDS is the squaring trick one harmonic up.
//     -> coherent demod   z = rds_bpf * conj(carrier)     (complex biphase)
//     -> LPF + decimate   channel_rate -> ~20 kHz working rate
//     -> biphase matched filter
//     -> bit-clock DDS (nominal = pilot / 16 = 1187.5 Hz) + Gardner timing
//     -> data-axis tracking (E[z^2]) + hard decision
//     -> differential decode     (removes the BPSK 180 degree ambiguity)
//     -> 26-bit block syndrome sync   (offset words A / B / C / C' / D)
//     -> group parse: PI, PS, PTY, TP/TA, RadioText (A/B clear), PTYN, CT
//
// Modelled on stereo_decoder.hpp: one class, built from the StationPlan,
// driven by process(disc, n) from MonoStation on the station-pool worker.
// Single-threaded; the mutex that publishes snapshot() to the status port
// lives in MonoStation.
#pragma once

#include <array>
#include <cmath>
#include <complex> // before <liquid/liquid.h>: makes liquid_float_complex == std::complex<float>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <liquid/liquid.h>

#include "dsp_util.hpp"
#include "plan.hpp"

namespace fmrx {

// --- the (26,16) shortened cyclic block code -------------------------
// g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1.  For a systematic cyclic
// code the syndrome of a received 26-bit block is just (block mod g); a
// valid block leaves a remainder equal to its position's offset word.
namespace rds {

constexpr uint16_t kGenLow = 0x1B9; // g(x) without the x^10 term
constexpr uint16_t kOffA = 0x0FC, kOffB = 0x198, kOffC = 0x168, kOffCp = 0x350,
                   kOffD = 0x1B4;
// index -> offset word; block position for each index is {0,1,2,2,3}.
constexpr std::array<uint16_t, 5> kOffsets{kOffA, kOffB, kOffC, kOffCp, kOffD};
constexpr std::array<int, 5> kOffsetPos{0, 1, 2, 2, 3};

// (block26 mod g), MSB-first LFSR division. Returns a 10-bit syndrome.
inline uint16_t syndrome(uint32_t block26) {
    uint16_t reg = 0;
    for (int i = 25; i >= 0; --i) {
        const uint16_t msb = (reg >> 9) & 1u;
        reg = static_cast<uint16_t>(((reg << 1) | ((block26 >> i) & 1u)) & 0x3FFu);
        if (msb)
            reg ^= kGenLow;
    }
    return reg;
}

// (data16 * x^10 mod g): the 10 check bits for a systematic block.
inline uint16_t checkbits(uint16_t data16) {
    uint16_t reg = 0;
    for (int i = 15; i >= 0; --i) {
        const uint16_t msb = (reg >> 9) & 1u;
        reg = static_cast<uint16_t>(((reg << 1) | ((data16 >> i) & 1u)) & 0x3FFu);
        if (msb)
            reg ^= kGenLow;
    }
    for (int i = 0; i < 10; ++i) {
        const uint16_t msb = (reg >> 9) & 1u;
        reg = static_cast<uint16_t>((reg << 1) & 0x3FFu);
        if (msb)
            reg ^= kGenLow;
    }
    return reg;
}

// A full 26-bit transmitted block: 16 data bits, then (checkword XOR offset).
inline uint32_t make_block(uint16_t data16, uint16_t offset) {
    return (static_cast<uint32_t>(data16) << 10) |
           static_cast<uint32_t>((checkbits(data16) ^ offset) & 0x3FFu);
}

// EN 50067 (RDS, not the US RBDS variant) programme-type names, index 0..31.
// Empty string for an out-of-range code.
inline const char* pty_name(int pty) {
    static const char* const kNames[32] = {
        "None",          "News",
        "Current affairs", "Information",
        "Sport",         "Education",
        "Drama",         "Culture",
        "Science",       "Varied",
        "Pop music",     "Rock music",
        "Easy listening", "Light classical",
        "Serious classical", "Other music",
        "Weather",       "Finance",
        "Children's programmes", "Social affairs",
        "Religion",      "Phone-in",
        "Travel",        "Leisure",
        "Jazz music",    "Country music",
        "National music", "Oldies music",
        "Folk music",    "Documentary",
        "Alarm test",    "Alarm"};
    return (pty >= 0 && pty < 32) ? kNames[pty] : "";
}

} // namespace rds

// --- decoded state exposed to the status port -----------------------
struct RdsPublic {
    bool lock = false;
    int pi = -1;   // programme identification, 16-bit; -1 until seen clean
    int pty = -1;  // programme type, 0..31
    bool tp = false;
    bool ta = false;
    std::string ps;      // 8-char programme service name (gated: full + stable)
    std::string rt;      // RadioText, <=64 chars (gated)
    std::string ptyn;    // 8-char programme type name (gated)
    std::string ct_iso;  // last valid clock-time, "YYYY-MM-DDTHH:MM:00Z" (UTC)
    float block_error_rate = 1.0f; // fraction of blocks failing the checkword
    float groups_per_sec = 0.0f;
    unsigned long long groups_ok = 0; // groups with all four blocks clean
};

class RdsDecoder {
public:
    explicit RdsDecoder(const StationPlan& sp) {
        // --- front-end decimation -------------------------------------------
        // RDS only needs the 19 kHz pilot and the 57 +/- 2.4 kHz subcarrier,
        // i.e. composite content to ~60 kHz. Running the 57/19 kHz band-pass
        // filters and the per-sample carrier recovery at the full channel
        // rate (up to ~640 kHz here) is ~10x more work than necessary --
        // both the sample count and, at a fixed transition width in Hz, the
        // filter lengths scale with fs. So decimate the raw discriminator to
        // a ~130-190 kHz working rate first; everything below derives its
        // rates from m_fs and follows automatically.
        const double ch_rate = static_cast<double>(sp.channel_rate_hz);
        const int fd =
            std::max(1, static_cast<int>(std::llround(ch_rate / 160000.0)));
        const double fs_dec = ch_rate / fd;
        // Only decimate when it leaves the Nyquist comfortably above the
        // 59.4 kHz RDS upper edge (>= ~140 kHz working rate). Otherwise run
        // at the full channel rate -- correct, just not cost-optimised for
        // that (uncommon) config.
        if (fd > 1 && fs_dec >= 140000.0) {
            m_front_decim = fd;
            m_fs = fs_dec;
            // Pass 0..60 kHz (pilot + RDS); stop under the post-decimation
            // Nyquist so SCA / HD sidebands above don't alias into the band.
            const double stop = std::min(0.5 * m_fs - 3000.0, 74000.0);
            m_front = RealLpfDecimator(ch_rate, 60000.0, stop, 60.0f,
                                       m_front_decim);
        } else {
            m_front_decim = 1;
            m_fs = ch_rate;
        }

        // Working rate ~20 kHz: 16-18 samples per 1187.5 bps bit. Any integer
        // decimation that lands close is fine -- the bit clock is recovered
        // downstream, it need not divide evenly.
        m_decim = std::max(1, static_cast<int>(std::lround(m_fs / 20000.0)));
        m_f2 = static_cast<double>(m_fs) / m_decim;

        // Gardner timing-error sign. The default (-1) locks the standard
        // positive-polarity discriminator composite (see --rds-selftest);
        // FMRX_RDS_TED_SIGN with a positive value flips it -- a field knob
        // in the same spirit as the now-removed FMRX_STEREO_INVERT, should
        // the carrier-cube phase or a reversed discriminator invert the
        // error slope.
        m_ted_sign = (std::getenv("FMRX_RDS_TED_SIGN") != nullptr &&
                      std::atoi(std::getenv("FMRX_RDS_TED_SIGN")) > 0)
                         ? 1.0f
                         : -1.0f;

        // 57 kHz complex band-pass: real Kaiser LPF prototype (unity DC gain),
        // heterodyned to +57 kHz so a real input yields the analytic +57 kHz
        // component -- exactly how the pilot BPF is built in stereo_decoder.
        m_bpf57 = make_bpf(2600.0, 6000.0, 57000.0);
        // 19 kHz pilot band-pass, same construction.
        m_bpf19 = make_bpf(1400.0, 5000.0, 19000.0);

        // Complex LPF + integer decimate of the coherent baseband. Stopband
        // well under the post-decimation Nyquist (~f2/2).
        {
            std::vector<float> h =
                kaiser_lpf_unity(m_fs, 2400.0, 6000.0, 50.0f);
            m_lpf = firfilt_crcf_create(h.data(),
                                        static_cast<unsigned int>(h.size()));
        }

        // Biphase matched filter at f2: +1 over the first half-bit, -1 over
        // the second. Length ~= one bit period.
        {
            m_sym_len = std::max(4, static_cast<int>(std::lround(m_f2 / 1187.5)));
            std::vector<float> h(m_sym_len, 0.0f);
            const int half = m_sym_len / 2;
            float norm = 0.0f;
            for (int i = 0; i < m_sym_len; ++i) {
                if (i < half)
                    h[i] = 1.0f;
                else if (i >= m_sym_len - half)
                    h[i] = -1.0f;
                norm += std::fabs(h[i]);
            }
            if (norm > 0.0f)
                for (float& v : h)
                    v /= norm;
            m_mf = firfilt_crcf_create(h.data(),
                                       static_cast<unsigned int>(h.size()));
        }

        m_pilot_dph = 2.0 * M_PI * 19000.0 / m_fs; // nominal, refined per block
        m_hist.assign(kHist, cfloat(0.0f, 0.0f));
    }

    RdsDecoder(const RdsDecoder&) = delete;
    RdsDecoder& operator=(const RdsDecoder&) = delete;
    ~RdsDecoder() {
        if (m_bpf57)
            firfilt_cccf_destroy(m_bpf57);
        if (m_bpf19)
            firfilt_cccf_destroy(m_bpf19);
        if (m_lpf)
            firfilt_crcf_destroy(m_lpf);
        if (m_mf)
            firfilt_crcf_destroy(m_mf);
    }

    int working_rate() const { return static_cast<int>(std::lround(m_f2)); }
    int sym_len() const { return m_sym_len; }
    bool locked() const { return m_state == State::Sync; }
    // Working-rate sample index (post front-end decimation) when block sync
    // first locked, or -1 if it never has.
    long long first_lock_sample() const { return m_lock_sample; }
    unsigned long long symbols() const { return m_sym_count; }
    unsigned long long groups_seen() const { return m_groups_seen; }

    // `disc` : raw discriminator output, `n` real samples at channel_rate.
    // Decimate to the working rate (if channel_rate warrants it), then run
    // the recovery chain on that.
    void process(const float* disc, size_t n) {
        if (m_front_decim <= 1) {
            process_core(disc, n);
            return;
        }
        m_dbuf.clear();
        m_front.process(disc, n, m_dbuf);
        process_core(m_dbuf.data(), m_dbuf.size());
    }

    // Recovery chain at m_fs (== channel_rate when no front-end decimation).
    void process_core(const float* disc, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const float x = disc[i];
            ++m_samples_total;

            cfloat s57, p19;
            firfilt_cccf_push(m_bpf57, cfloat(x, 0.0f));
            firfilt_cccf_execute(m_bpf57, &s57);
            firfilt_cccf_push(m_bpf19, cfloat(x, 0.0f));
            firfilt_cccf_execute(m_bpf19, &p19);

            // pilot power vs input power (dBc EMA) -> a coarse presence flag.
            m_pil_pow += kPowA * (static_cast<double>(std::norm(p19)) - m_pil_pow);
            m_in_pow += kPowA * (static_cast<double>(x) * x - m_in_pow);

            // unit-magnitude pilot, cubed -> phase-locked 57 kHz reference.
            const float mag = std::sqrt(std::norm(p19)) + 1e-12f;
            const cfloat pn = p19 / mag;
            const cfloat carrier = pn * pn * pn;

            // track the pilot's phase rate (rad/sample) for the bit-clock DDS.
            const float dph = std::arg(pn * std::conj(m_pn_prev));
            m_pn_prev = pn;
            m_pilot_dph += kDphA * (dph - m_pilot_dph);

            // coherent demod -> complex biphase baseband at channel_rate.
            const cfloat z = s57 * std::conj(carrier);

            // LPF + decimate to the working rate.
            firfilt_crcf_push(m_lpf, z);
            if (--m_decim_phase <= 0) {
                m_decim_phase = m_decim;
                cfloat zd;
                firfilt_crcf_execute(m_lpf, &zd);
                feed_symbol_rate(zd);
            }
        }
    }

    RdsPublic snapshot() const {
        RdsPublic p;
        p.lock = (m_state == State::Sync);
        p.pi = m_pi;
        p.pty = m_pty;
        p.tp = m_tp;
        p.ta = m_ta;
        // PS / RT are held across brief re-syncs (a fady signal drops lock
        // for a second or two constantly) but suppressed once sync has been
        // gone long enough that the text is probably stale -- e.g. the user
        // retuned to a dead frequency.
        const bool stale =
            m_unlock_sample >= 0 &&
            static_cast<double>(m_samples_total - m_unlock_sample) > 5.0 * m_fs;
        p.ps = stale ? std::string() : m_ps_pub;
        p.rt = stale ? std::string() : m_rt_pub;
        p.ptyn = m_ptyn_pub;
        p.ct_iso = m_ct_iso;
        p.block_error_rate = m_ber;
        p.groups_per_sec = m_gps;
        p.groups_ok = m_groups_ok;
        return p;
    }

private:
    // ---- construction helpers ----
    firfilt_cccf make_bpf(double pass_hz, double stop_hz, double center_hz) {
        std::vector<float> lp = kaiser_lpf_unity(m_fs, pass_hz, stop_hz, 55.0f);
        std::vector<cfloat> bp(lp.size());
        const double w = 2.0 * M_PI * center_hz / m_fs;
        for (size_t i = 0; i < lp.size(); ++i)
            bp[i] = lp[i] * cfloat(static_cast<float>(std::cos(w * i)),
                                   static_cast<float>(std::sin(w * i)));
        return firfilt_cccf_create(bp.data(),
                                   static_cast<unsigned int>(bp.size()));
    }

    // ---- working-rate symbol recovery ----
    static constexpr int kHist = 64; // ring for sub-sample interpolation (pow2)

    cfloat hist_at(double t) const {
        const double fi = std::floor(t);
        const long i0 = static_cast<long>(fi);
        const cfloat a = m_hist[static_cast<size_t>(i0) & (kHist - 1)];
        const cfloat b = m_hist[static_cast<size_t>(i0 + 1) & (kHist - 1)];
        return a + (b - a) * static_cast<float>(t - fi);
    }

    void feed_symbol_rate(cfloat zd) {
        cfloat mf;
        firfilt_crcf_push(m_mf, zd);
        firfilt_crcf_execute(m_mf, &mf);
        m_hist[m_hn & (kHist - 1)] = mf;
        ++m_hn;
        if (m_hn < kHist)
            return;

        // bit-clock DDS: nominal rate from the tracked pilot (pilot / 16),
        // plus a small Gardner-driven trim.
        const double pilot_hz = m_pilot_dph * m_fs / (2.0 * M_PI);
        double base = (pilot_hz > 17000.0 && pilot_hz < 21000.0)
                          ? pilot_hz / 16.0
                          : 1187.5;
        double bias = m_bit_bias;
        if (bias > 3.0)
            bias = 3.0;
        else if (bias < -3.0)
            bias = -3.0;
        const double dbeta = (base + bias) / m_f2;

        m_beta += dbeta;
        if (m_beta < 1.0)
            return;
        m_beta -= 1.0;

        // strobe fell between the last two history samples.
        const double t_strobe = static_cast<double>(m_hn - 1) - m_beta / dbeta;
        const cfloat zk = hist_at(t_strobe);
        const cfloat zmid = hist_at(t_strobe - 0.5 / dbeta); // Gardner midpoint

        // data-axis estimate from E[z^2] (immune to the +/- data sign).
        float psi = 0.5f * std::atan2(m_axis.imag(), m_axis.real());
        const cfloat rot = std::polar(1.0f, -psi);
        const float yk = (zk * rot).real();
        const float ymid = (zmid * rot).real();

        // normalised Gardner timing error, fed back to phase + rate.
        const float denom = yk * yk + m_yprev * m_yprev + 1e-6f;
        const float e = m_ted_sign * ymid * (yk - m_yprev) / denom;
        m_beta -= kTedKp * e;
        m_bit_bias += kTedKi * e * base; // e is dimensionless; scale to Hz
        m_yprev = yk;

        m_axis = m_axis * (1.0f - kAxisA) + (zk * zk) * kAxisA;

        const int dbit = (yk >= 0.0f) ? 1 : 0;
        const int data = dbit ^ m_dbit_prev;
        m_dbit_prev = dbit;
        ++m_sym_count;

        block_sync_push(data);
    }

    // ---- 26-bit block / group synchroniser ----
    enum class State { Acquire, Sync };

    static int match_offset(uint16_t syn) {
        for (int i = 0; i < 5; ++i)
            if (syn == rds::kOffsets[i])
                return i;
        return -1;
    }

    void block_sync_push(int data_bit) {
        m_reg = ((m_reg << 1) | (data_bit & 1u)) & 0x3FFFFFFu;
        ++m_gbits;

        if (m_state == State::Acquire) {
            const int oi = match_offset(rds::syndrome(m_reg));
            if (oi >= 0) {
                m_acq.push_back({oi, m_gbits});
                while (!m_acq.empty() && m_acq.front().second < m_gbits - 130)
                    m_acq.pop_front();
                if (oi == 4) // a D match -- try to anchor a full group behind it
                    try_lock();
            }
            return;
        }

        // --- tracking: a block boundary every 26 bits ---
        if (++m_bit_since < 26)
            return;
        m_bit_since = 0;

        const uint16_t syn = rds::syndrome(m_reg);
        const int pos = m_blockpos;
        bool ok;
        if (pos == 2)
            ok = (syn == rds::kOffC || syn == rds::kOffCp);
        else
            ok = (syn == rds::kOffsets[pos == 3 ? 4 : pos]);

        m_blk[pos] = static_cast<uint16_t>(m_reg >> 10);
        m_blk_ok[pos] = ok;
        m_ber += kBerA * ((ok ? 0.0f : 1.0f) - m_ber);

        if (ok) {
            m_bad_run = 0;
        } else if (++m_bad_run > kMaxBadBlocks) {
            m_state = State::Acquire;
            m_acq.clear();
            if (m_unlock_sample < 0)
                m_unlock_sample = static_cast<long long>(m_samples_total);
            return;
        }

        if (pos == 3) {
            on_group();
            ++m_groups_seen;
            if (m_blk_ok[0] && m_blk_ok[1] && m_blk_ok[2] && m_blk_ok[3])
                ++m_groups_ok;
            const double now = static_cast<double>(m_samples_total) / m_fs;
            if (m_last_group_t > 0.0) {
                const double inst = 1.0 / std::max(now - m_last_group_t, 1e-6);
                m_gps += 0.05f * (static_cast<float>(inst) - m_gps);
            }
            m_last_group_t = now;
        }
        m_blockpos = (m_blockpos + 1) & 3;
    }

    // Anchor on the most recent D match: require C/C' 26 bits back, B at 52,
    // A at 78. False-lock probability ~ (1/1024)^3.
    void try_lock() {
        const long long d_bit = m_gbits;
        auto has = [&](int want_pos, long long at) {
            for (const auto& m : m_acq)
                if (m.second == at && rds::kOffsetPos[m.first] == want_pos)
                    return true;
            return false;
        };
        if (!has(2, d_bit - 26) || !has(1, d_bit - 52) || !has(0, d_bit - 78))
            return;

        m_state = State::Sync;
        m_blockpos = 0; // the D bit we just consumed was a block boundary
        m_bit_since = 0;
        m_bad_run = 0;
        m_acq.clear();
        if (m_lock_sample < 0)
            m_lock_sample = static_cast<long long>(m_samples_total);
        // Re-locked after a long silence -> the held PS / RT are from a
        // different tuning; drop them and start clean. A brief drop (a fade)
        // keeps them.
        if (m_unlock_sample >= 0) {
            const double gap =
                static_cast<double>(m_samples_total - m_unlock_sample);
            if (gap > 5.0 * m_fs) {
                m_ps_pub.clear();
                m_ps_cand.clear();
                m_rt_pub.clear();
                m_rt_cand.clear();
                m_rt_bank[0].reset();
                m_rt_bank[1].reset();
            }
            m_unlock_sample = -1;
        }
    }

    // ---- group parsing ----
    static char sanitize(int c) {
        return (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
    }

    void on_group() {
        const uint16_t* b = m_blk;
        const bool* ok = m_blk_ok;
        if (ok[0])
            m_pi = b[0];
        if (!ok[1])
            return;

        const int gt = b[1] >> 12;
        const int ver = (b[1] >> 11) & 1; // 0 = version A, 1 = version B
        m_tp = (b[1] >> 10) & 1;
        m_pty = (b[1] >> 5) & 0x1F;
        if (ver == 1 && ok[2])
            m_pi = b[2]; // block C repeats PI in version B

        switch (gt) {
        case 0: { // 0A / 0B -- programme service name
            m_ta = (b[1] >> 4) & 1;
            const int seg = b[1] & 0x03;
            if (ok[3]) {
                put_ps(seg * 2, (b[3] >> 8) & 0xFF);
                put_ps(seg * 2 + 1, b[3] & 0xFF);
            }
            break;
        }
        case 2: { // 2A / 2B -- RadioText
            // The A/B flag just selects which accumulation bank the
            // segments land in -- no wipe here. put_rt() detects a genuine
            // text change (a clean char that differs from one we held) and
            // resets that bank itself; m_rt_pub (what the status port
            // shows) is held until the new text is mostly assembled.
            m_rt_ab = (b[1] >> 4) & 1;
            const int seg = b[1] & 0x0F;
            if (ver == 0) {
                if (ok[2]) {
                    put_rt(seg * 4, (b[2] >> 8) & 0xFF);
                    put_rt(seg * 4 + 1, b[2] & 0xFF);
                }
                if (ok[3]) {
                    put_rt(seg * 4 + 2, (b[3] >> 8) & 0xFF);
                    put_rt(seg * 4 + 3, b[3] & 0xFF);
                }
            } else if (ok[3]) {
                put_rt(seg * 2, (b[3] >> 8) & 0xFF);
                put_rt(seg * 2 + 1, b[3] & 0xFF);
            }
            break;
        }
        case 4: // 4A -- clock time
            if (ver == 0 && ok[2] && ok[3])
                parse_ct(b);
            break;
        case 10: // 10A -- programme type name
            if (ver == 0) {
                const int seg = b[1] & 0x01;
                if (ok[2]) {
                    put_ptyn(seg * 4, (b[2] >> 8) & 0xFF);
                    put_ptyn(seg * 4 + 1, b[2] & 0xFF);
                }
                if (ok[3]) {
                    put_ptyn(seg * 4 + 2, (b[3] >> 8) & 0xFF);
                    put_ptyn(seg * 4 + 3, b[3] & 0xFF);
                }
            }
            break;
        default:
            break;
        }
    }

    // PS: surface only once all 8 positions are filled AND two consecutive
    // full copies agree -- erring toward "blank" over a wrong name.
    void put_ps(int idx, int ch) {
        if (idx < 0 || idx > 7)
            return;
        m_ps_raw[idx] = sanitize(ch);
        m_ps_seen |= (1u << idx);
        if (m_ps_seen != 0xFF)
            return;
        std::string cand(m_ps_raw, m_ps_raw + 8);
        rstrip(cand);
        if (cand == m_ps_cand)
            m_ps_pub = cand;
        else
            m_ps_cand = cand;
    }

    // RadioText, accumulated per A/B bank across many transmissions. A
    // fady station rarely lands all 16 segments in one clean pass, and it
    // rewrites RT (flipping A/B) faster than a from-scratch reassemble can
    // keep up -- so segments persist in the bank, m_rt_pub is only ever
    // *replaced*, never blanked on a flip, and a partial is revealed once
    // it's most of the way there (approach (a): the previous text stays up
    // until then).
    static constexpr int kRtRevealMinChars = 12; // don't show a shorter fragment
    static constexpr int kRtRevealNum = 3;       // ...unless >= 3/4 of the message
    static constexpr int kRtRevealDen = 4;

    void put_rt(int idx, int ch) {
        if (idx < 0 || idx > 63)
            return;
        RtBank& bk = m_rt_bank[m_rt_ab & 1];
        const char c = static_cast<char>(ch);
        // A clean block decoding a *different* char where we already held
        // one: this bank now carries a new message -- start it over.
        if ((bk.seen >> idx & 1) && bk.raw[idx] != c)
            bk.reset();
        bk.raw[idx] = c;
        bk.seen |= (1ull << idx);

        // message length: through the 0x0D terminator once seen, else 64
        int term = -1;
        for (int i = 0; i < 64; ++i)
            if ((bk.seen >> i & 1) && bk.raw[i] == 0x0D) {
                term = i;
                break;
            }
        const int len = term >= 0 ? term : 64;

        // longest gap-free prefix currently held
        int prefix = 0;
        while (prefix < len && (bk.seen >> prefix & 1))
            ++prefix;

        std::string cand;
        for (int i = 0; i < prefix; ++i)
            cand.push_back(sanitize(bk.raw[i]));
        rstrip(cand);

        if (prefix >= len) {
            // whole message in hand -- keep the two-pass confirm for a
            // *change* (anti-garble); the first publish after (re)lock is
            // immediate.
            if (cand == m_rt_cand || m_rt_pub.empty())
                m_rt_pub = cand;
            m_rt_cand = cand;
        } else if (prefix >= kRtRevealMinChars &&
                   prefix * kRtRevealDen >= len * kRtRevealNum &&
                   cand.size() > m_rt_pub.size()) {
            // reveal the growing prefix only once it's ~3/4 there and only
            // when it extends what's shown -- never shrink to a fragment
            // mid-fill. Otherwise the previous m_rt_pub stays up.
            m_rt_pub = cand;
        }
    }

    void put_ptyn(int idx, int ch) {
        if (idx < 0 || idx > 7)
            return;
        m_ptyn_raw[idx] = sanitize(ch);
        m_ptyn_seen |= (1u << idx);
        if (m_ptyn_seen != 0xFF)
            return;
        std::string cand(m_ptyn_raw, m_ptyn_raw + 8);
        rstrip(cand);
        if (cand == m_ptyn_cand)
            m_ptyn_pub = cand;
        else
            m_ptyn_cand = cand;
    }

    void parse_ct(const uint16_t b[4]) {
        const uint32_t mjd =
            (static_cast<uint32_t>(b[1] & 0x03) << 15) | (b[2] >> 1);
        const int hour = ((b[2] & 0x01) << 4) | (b[3] >> 12);
        const int minute = (b[3] >> 6) & 0x3F;
        if (hour > 23 || minute > 59 || mjd < 15079)
            return;
        // MJD -> Gregorian (EN 50067 Annex G).
        const int yp = static_cast<int>((mjd - 15078.2) / 365.25);
        const int mp = static_cast<int>(
            (mjd - 14956.1 - static_cast<int>(yp * 365.25)) / 30.6001);
        const int day = static_cast<int>(mjd) - 14956 -
                        static_cast<int>(yp * 365.25) -
                        static_cast<int>(mp * 30.6001);
        const int k = (mp == 14 || mp == 15) ? 1 : 0;
        const int year = yp + k + 1900;
        const int month = mp - 1 - k * 12;
        if (month < 1 || month > 12 || day < 1 || day > 31)
            return;
        char buf[40];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:00Z", year,
                      month, day, hour, minute);
        m_ct_iso = buf;
    }

    static void rstrip(std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' ||
                              s.back() == '\n' || s.back() == '\0'))
            s.pop_back();
    }

    // ---- loop constants ----
    static constexpr double kPowA = 0.0005;  // pilot/input power EMA
    static constexpr double kDphA = 0.0005;  // pilot phase-rate EMA
    static constexpr float kAxisA = 0.02f;   // data-axis E[z^2] EMA
    static constexpr float kTedKp = 0.20f;   // Gardner proportional
    static constexpr float kTedKi = 0.002f;  // Gardner integral (-> rate bias)
    static constexpr float kBerA = 0.02f;    // block-error-rate EMA
    static constexpr int kMaxBadBlocks = 12; // ~3 groups before dropping sync

    // ---- front-end ----
    RealLpfDecimator m_front;     // raw disc @ channel_rate -> m_fs (if decimating)
    std::vector<float> m_dbuf;    // scratch for one block of decimated disc
    int m_front_decim = 1;
    double m_fs = 0.0;           // working rate: channel_rate / m_front_decim
    int m_decim = 1;
    double m_f2 = 0.0;
    int m_sym_len = 0;
    float m_ted_sign = 1.0f;

    firfilt_cccf m_bpf57 = nullptr;
    firfilt_cccf m_bpf19 = nullptr;
    firfilt_crcf m_lpf = nullptr;
    firfilt_crcf m_mf = nullptr;
    int m_decim_phase = 1;

    cfloat m_pn_prev{1.0f, 0.0f};
    double m_pilot_dph = 0.0;
    double m_pil_pow = 0.0, m_in_pow = 0.0;

    // ---- symbol clock / decision ----
    std::vector<cfloat> m_hist;
    unsigned long long m_hn = 0;
    double m_beta = 0.0;
    double m_bit_bias = 0.0;
    float m_yprev = 0.0f;
    cfloat m_axis{0.0f, 0.0f};
    int m_dbit_prev = 0;
    unsigned long long m_sym_count = 0;

    // ---- block sync ----
    State m_state = State::Acquire;
    uint32_t m_reg = 0;
    long long m_gbits = 0;
    std::deque<std::pair<int, long long>> m_acq;
    int m_blockpos = 0;
    int m_bit_since = 0;
    int m_bad_run = 0;
    uint16_t m_blk[4] = {0, 0, 0, 0};
    bool m_blk_ok[4] = {false, false, false, false};
    long long m_lock_sample = -1;
    long long m_unlock_sample = -1; // m_samples_total when sync was last lost
    unsigned long long m_samples_total = 0;

    // ---- decoded fields ----
    int m_pi = -1, m_pty = -1;
    bool m_tp = false, m_ta = false;

    char m_ps_raw[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    unsigned m_ps_seen = 0;
    std::string m_ps_cand, m_ps_pub;

    // One RadioText accumulation buffer per A/B group flag -- see put_rt.
    struct RtBank {
        char raw[64];
        uint64_t seen = 0;
        RtBank() { std::memset(raw, ' ', sizeof raw); }
        void reset() {
            std::memset(raw, ' ', sizeof raw);
            seen = 0;
        }
    };
    RtBank m_rt_bank[2];
    int m_rt_ab = -1;               // which bank the current 2A/2B group feeds
    std::string m_rt_cand, m_rt_pub;

    char m_ptyn_raw[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    unsigned m_ptyn_seen = 0;
    std::string m_ptyn_cand, m_ptyn_pub;

    std::string m_ct_iso;

    // ---- health ----
    float m_ber = 1.0f;
    float m_gps = 0.0f;
    double m_last_group_t = 0.0;
    unsigned long long m_groups_seen = 0, m_groups_ok = 0;
};

} // namespace fmrx
