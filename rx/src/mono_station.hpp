// One mono FM station's full per-channel chain:
//
//   bin samples @ channel_rate (complex)
//     -> fine mixer (nco_crcf, shift station to DC)
//     -> pre-demod adjacent-channel LPF (firfilt_crcf)
//     -> FM discriminator  (arg(x[n] * conj(x[n-1])) * k)
//     -> de-emphasis IIR   (single pole, coeffs from the plan)
//     -> audio stage A LPF + decimate  channel_rate -> fm_intermediate
//     -> audio stage B LPF + decimate  fm_intermediate -> audio_rate
//     -> soft AGC  (optional, level the output)
//     -> squelch   (optional, mute when the noise-ratio metric says no signal)
//     -> int16 + UDP
//
// A station with stereo_mode != Mono skips the de-emphasis / audio-stage /
// AGC path above after the discriminator: emit_stereo() hands the raw
// discriminator output to a StereoDecoder (stereo_decoder.hpp) instead,
// which does its own audio filtering, L/R matrix and per-channel
// de-emphasis, then this class's AGC (stereo variant) / squelch / UDP.
//
// Filter taps come from liquid's Kaiser designer (see dsp_util.hpp);
// tune and verify the chain by ear / spectrum, not against a reference
// implementation.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <liquid/liquid.h>

#include "dsp_util.hpp"
#include "plan.hpp"
#include "rds_decoder.hpp"
#include "stereo_decoder.hpp"
#include "udp_sink.hpp"

namespace fmrx {

class MonoStation {
public:
    // capture_only: skip the UDP send (and AGC / squelch, which are about
    // levels not DSP correctness) and instead append every PCM sample to
    // the public `captured` buffer. Used by --selftest / --sweep.
    MonoStation(const StationPlan& sp, const std::string& udp_host, int udp_mtu,
                bool capture_only = false)
        : m_label(sp.label),
          m_capture_only(capture_only),
          m_k(static_cast<double>(sp.channel_rate_hz) /
              (2.0 * M_PI * sp.fm_deviation_hz)),
          m_deemph_b0(static_cast<float>(sp.deemph_b0)),
          m_deemph_p1(static_cast<float>(sp.deemph_p1)),
          m_sink(udp_host, sp.port, udp_mtu, sp.stereo ? 4 : 2) {
        const float ch_rate = static_cast<float>(sp.channel_rate_hz);

        // Fine mixer: shift the station from +fine_hz down to DC
        // (osc = exp(-j * 2pi fine_hz/ch_rate * t)).
        m_nco = nco_crcf_create(LIQUID_VCO);
        nco_crcf_set_frequency(
            m_nco, static_cast<float>(2.0 * M_PI * sp.fine_hz / ch_rate));

        // Pre-demod adjacent-channel LPF: passband = Carson half-BW,
        // transition = fm.predemod_transition. Designed + unity-normalised
        // by hand rather than via firfilt_crcf_create_kaiser(), which
        // (like liquid_firdes_kaiser) leaves a ~3x DC gain on the prototype.
        {
            const double pass = sp.fm_deviation_hz + sp.fm_audio_bw_hz;
            std::vector<float> h = kaiser_lpf_unity(
                ch_rate, pass, pass + sp.fm_predemod_transition_hz, 60.0f);
            m_predemod =
                firfilt_crcf_create(h.data(), static_cast<unsigned int>(h.size()));
        }

        // Audio stage A: channel_rate -> fm_intermediate_rate.
        m_stage_a = RealLpfDecimator(ch_rate, sp.fm_audio_bw_hz,
                                     sp.fm_audio_stop_hz, 60.0f, sp.fm_decim);

        // Audio stage B: fm_intermediate_rate -> audio_rate.
        m_stage_b =
            RealLpfDecimator(sp.fm_intermediate_rate_hz, sp.fm_audio_bw_hz,
                             sp.fm_audio_stop_hz, 60.0f, sp.audio_decim);

        // --- soft AGC (on the final audio) ---
        m_agc_enabled = sp.agc_enabled;
        m_agc_target = static_cast<float>(sp.agc_target);
        m_agc_max_gain =
            std::pow(10.0f, static_cast<float>(sp.agc_max_gain_db) / 20.0f);
        const double afs = sp.audio_rate_hz;
        m_agc_rms_alpha = alpha_from_ms(sp.agc_response_ms, afs);
        m_agc_attack_alpha =
            alpha_from_ms(std::min(sp.agc_response_ms * 0.1, 20.0), afs);
        m_agc_release_alpha = alpha_from_ms(sp.agc_response_ms, afs);
        m_agc_rms2 = m_agc_target * m_agc_target;
        m_agc_gain = 1.0f;

        // --- soft-mute ramp -------------------------------------------
        // A one-pole gain envelope toward (squelch-open ? 1 : 0), applied to
        // the final audio instead of a hard `sample = 0`. ~12 ms to
        // open, ~25 ms to close -- fast enough not to chop speech, slow
        // enough to kill the click/thump on every squelch edge and on
        // retune. Starts at 0 so the first audio after start/reload fades
        // in instead of popping.
        m_mute_attack_a = alpha_from_ms(12.0, afs);
        m_mute_release_a = alpha_from_ms(25.0, afs);

        // --- progressive high-cut ---------------------------------------
        // Sweep a one-pole LPF corner on the final audio from the full
        // audio bandwidth (transparent, high C/N) down toward ~4.5 kHz as
        // the calibrated snr_db falls, to bury weak-signal hiss. Driven off
        // the same stable snr_db as the stereo blend. The corner is
        // slew-limited block-to-block so it never whooshes.
        m_hc_audio_bw = static_cast<float>(sp.fm_audio_bw_hz);
        m_hc_fs = static_cast<float>(sp.audio_rate_hz);
        m_hc_max_hz = std::min(2.0f * m_hc_audio_bw, 0.45f * m_hc_fs);
        m_hc_corner = m_hc_max_hz; // start transparent
        m_hc_a = 1.0f;
        m_high_cut_hz.store(m_hc_audio_bw);

        // --- 19 kHz pilot notch -----------------------------------------
        // A dedicated narrow notch at the stereo pilot on the final audio,
        // so it never reaches the encoder to beat against sample-rate
        // artefacts. Inert unless the audio rate lifts 19 kHz clear of
        // Nyquist -- so dormant on the 32 kHz baseline (where the two audio
        // stages already put the pilot >100 dB down before it folds) and
        // live once audio.rate goes to 44.1 / 48 kHz.
        m_pilot_notch = BiquadNotch(afs, kPilotNotchHz, kPilotNotchQ);
        m_pilot_notch_r = BiquadNotch(afs, kPilotNotchHz, kPilotNotchQ);

        // --- look-ahead peak limiter -------------------------------------
        // The final peak stage before int16, avoiding the harmonic
        // distortion a memoryless tanh soft-clip would add on every
        // transient. ~1.5 ms look-ahead, ~60 ms release, ceiling a hair
        // below full scale. Stereo limits L/R on one shared gain.
        m_limiter = LookaheadLimiter(afs, kLimLookaheadMs, kLimReleaseMs,
                                     kLimCeiling, sp.stereo ? 2 : 1);

        // --- weak-signal demod -------------------------------------------
        // fm.demod: pll  -> a feedback FM demodulator (mono only) whose
        // loop-filter noise bandwidth is narrower than the open-loop atan2
        // detector's, for ~2-3 dB of threshold extension. fm.declick -> an
        // impulsive-click suppressor on the discriminator output. Both are
        // off by default and both run in capture_only (demod correctness,
        // not level shaping).
        // Env FMRX_FM_DEMOD / FMRX_FM_DECLICK override the resolved value,
        // mirroring FMRX_SNR_KNOISE, so --demod-cal can compare modes
        // without a rebuild.
        {
            const char* dm = std::getenv("FMRX_FM_DEMOD");
            m_demod_mode = dm ? std::string(dm) : sp.fm_demod;
            const char* dc = std::getenv("FMRX_FM_DECLICK");
            const bool declick = dc ? (std::atoi(dc) != 0) : sp.fm_declick;
            if (m_demod_mode == "pll" &&
                sp.stereo_mode == StereoMode::Mono) {
                const double bw =
                    sp.fm_pll_bw_hz > 0.0 ? sp.fm_pll_bw_hz : kPllDemodBwHz;
                m_pll_demod = std::make_unique<PllFmDemod>(ch_rate, bw);
            } else {
                m_demod_mode = "discriminator";
            }
            if (declick)
                m_declicker = std::make_unique<Declicker>(
                    ch_rate, static_cast<float>(sp.fm_declick_sigma),
                    kDeclickFloor);
        }

        // AFC carrier-drift tracking. On unless `fm.afc: false` (or the
        // FMRX_NO_AFC env) pins the fine mixer static -- for a station
        // whose discriminator DC mean is too multipath-corrupted for the
        // loop to track without warbling the demod.
        m_afc_enabled = std::getenv("FMRX_NO_AFC") ? false : sp.fm_afc;

        // Look-ahead limiter. On unless `fm.limiter: false` (or the
        // FMRX_NO_LIMITER env) -- then the memoryless tanh soft-clip in
        // the AGC is the sole peak stage.
        m_limiter_enabled =
            std::getenv("FMRX_NO_LIMITER") ? false : sp.fm_limiter;

        // --- multipath indicator -----------------------------------------
        // Track the pre-demod IF envelope: constant-modulus for a clean FM
        // carrier, ripples with frequency-selective multipath fading. The
        // ripple relative to the (slow) envelope mean is published as a
        // 0..100 "%" AM-depth figure. Indicator only -- not (yet) wired
        // into the blend / high-cut. Slow DC tracker corner ~1/(2*pi*tau).
        m_mp_dc_a = alpha_from_ms(kMultipathDcTauMs, ch_rate);

        // --- AFC / carrier-drift tracking --------------------------------
        // The pre-de-emphasis discriminator output is in units of
        // fm_deviation, so its block mean * fm_deviation_hz is the residual
        // carrier-frequency error in Hz. Integrate a fraction of it back
        // into the fine mixer each block, clamped to a few kHz, frozen
        // while squelched and during a startup warmup. Tracks the LibreSDR
        // TCXO drift (ppm-level, sun-exposed enclosure) and stations sitting
        // slightly off nominal; also gives a "tuning error" readout.
        m_fine_hz = sp.fine_hz;
        m_afc_dev_hz = static_cast<float>(sp.fm_deviation_hz);
        m_afc_ch_rate = ch_rate;
        // ~0.75 s of samples: let the guard-band SNR EMA and the filter
        // transients settle before AFC / high-cut start reacting.
        m_warmup_samp = static_cast<long long>(0.75 * ch_rate);

        // --- squelch metric: FM noise power above ~60 kHz in the
        // discriminator output (well clear of audio / 19 kHz pilot / 57 kHz
        // RDS), vs total demod power. Ratio is tiny for a locked carrier,
        // large for open-squelch FM hiss. Butterworth HP, run at
        // channel_rate on the raw (pre de-emphasis) discriminator output.
        m_sq_enabled = sp.squelch_enabled;
        m_sq_open_snr_db = static_cast<float>(sp.squelch_open_snr_db);
        m_sq_hang_samp = static_cast<long long>(sp.squelch_hang_ms / 1000.0 *
                                                sp.channel_rate_hz);
        m_sq_hang_left = m_sq_hang_samp;
        const float hp_fc = 60000.0f / ch_rate;
        m_hpf = iirfilt_rrrf_create_prototype(
            LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_HIGHPASS, LIQUID_IIRDES_SOS,
            5, hp_fc, 0.0f, 1.0f, 60.0f);

        // --- calibrated SNR readout ---------------------------------------
        // A physically-referenced, program-independent SNR: estimate the FM
        // discriminator's noise PSD from an out-of-band guard region (where
        // only the FM hiss floor lives) and compare it, integrated through
        // de-emphasis over the audio band, to a fixed reference-deviation
        // sine. This is how bench gear measures FM-tuner SNR, and unlike the
        // squelch proxy it does not move with modulation level.
        //
        // The discriminator output here is already normalised to units of
        // fm_deviation (d = f_inst / deviation), so a peak-deviation sine has
        // power 0.5 -- that is P_ref. Noise model across the guard band
        // [f1,f2]:  S(f) = N0 * f^2  (textbook parabolic FM discriminator
        // noise; the phase-increment detector is flat so no |H_disc|^2
        // correction). N0 = 3 * P_g / (f2^3 - f1^3).
        m_channel_rate = ch_rate;
        {
            // Guard band: a clean 12 kHz window just below the pre-demod
            // passband edge (deviation + audio_bw), so |H_predemod|^2 ~ 1
            // across it. For the 75 kHz / 15 kHz baseline that is 76-88 kHz
            // -- above the 57 kHz RDS subcarrier (59.4 kHz upper sideband)
            // and the 67 kHz SCA, below the 90 kHz pre-demod edge. An
            // elliptic HP+LP cascade (liquid's BANDPASS prototype is
            // numerically degenerate at this ~2% fractional bandwidth) puts
            // all of RDS / pilot / stereo subcarrier / SCA >= 60 dB down.
            // resolve_config has already hard-errored if this window can't
            // be placed (f1 >= 68 kHz, f2 < 0.45 channel_rate), so it is
            // always built -- there is no proxy fallback.
            const double predemod_pass =
                static_cast<double>(sp.fm_deviation_hz) + sp.fm_audio_bw_hz;
            const double f2 = predemod_pass - 2000.0;
            const double f1 = f2 - 12000.0;
            const double audio_top = static_cast<double>(sp.fm_audio_bw_hz);

            m_inv_guard_integral = 3.0 / (f2 * f2 * f2 - f1 * f1 * f1);
            // In-band noise power through de-emphasis:
            //   integral_0^audio_top  f^2 / (1 + (2*pi*f*tau)^2) df
            // closed form with a = 2*pi*tau:
            //   F(f) = f/a^2 - atan(a*f)/a^3
            const double a = 2.0 * M_PI * sp.fm_tau_s;
            auto Fd = [a](double f) {
                return f / (a * a) - std::atan(a * f) / (a * a * a);
            };
            m_inband_deemph_integral = Fd(audio_top) - Fd(0.0);
            m_snr_pref = 0.5; // peak-deviation reference sine, normalised units

            m_gb_hp = iirfilt_rrrf_create_prototype(
                LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_HIGHPASS,
                LIQUID_IIRDES_SOS, 8, static_cast<float>(f1 / ch_rate), 0.0f,
                0.5f, 60.0f);
            m_gb_lp = iirfilt_rrrf_create_prototype(
                LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS,
                LIQUID_IIRDES_SOS, 8, static_cast<float>(f2 / ch_rate), 0.0f,
                0.5f, 60.0f);

            // K_noise precedence: FMRX_SNR_KNOISE env > config snr.k_noise >
            // the value fitted into the binary.
            const char* kn = std::getenv("FMRX_SNR_KNOISE");
            m_snr_knoise = kn ? static_cast<float>(std::atof(kn))
                          : sp.snr_knoise > 0.0
                              ? static_cast<float>(sp.snr_knoise)
                              : kSnrKNoiseDefault;
        }

        // --- stereo multiplex decoder ---
        // Runs off the raw discriminator output (m_disc). When present the
        // mono de-emphasis / audio stages below are bypassed. In
        // capture_only mode it still runs (so --stereo-selftest can drive
        // it); AGC / squelch stay off there, same as the mono capture path.
        if (sp.stereo_mode != StereoMode::Mono)
            m_stereo = std::make_unique<StereoDecoder>(sp);

        // --- RDS (Radio Data System) decoder ---
        // Runs off the raw discriminator output (m_disc). Requires
        // channel_rate >= 120 kHz (composite must reach ~59.4 kHz); the
        // decoder decimates that to a ~160 kHz working rate internally.
        // Only active when station is in the listener-gated active set
        // (bounded CPU cost: ~+1-2 pp per active station).
        // Disabled in capture_only mode (no active-set mechanism there).
        if (sp.rds_enabled && sp.channel_rate_hz >= 120000 && !capture_only)
            m_rds = std::make_unique<RdsDecoder>(sp);
    }

    MonoStation(const MonoStation&) = delete;
    MonoStation& operator=(const MonoStation&) = delete;

    ~MonoStation() {
        if (m_nco)
            nco_crcf_destroy(m_nco);
        if (m_predemod)
            firfilt_crcf_destroy(m_predemod);
        if (m_hpf)
            iirfilt_rrrf_destroy(m_hpf);
        if (m_gb_hp)
            iirfilt_rrrf_destroy(m_gb_hp);
        if (m_gb_lp)
            iirfilt_rrrf_destroy(m_gb_lp);
    }

    const std::string& label() const { return m_label; }

    // live signal metadata (updated every process() call), for the status
    // port. Meaningless in capture_only mode.
    float rf_dbfs() const { return m_rf_dbfs.load(); }
    // Calibrated, program-independent SNR (dB), comparable to bench FM-tuner
    // specs -- see "calibrated SNR readout" below. Always the calibrated
    // figure; resolve_config guarantees the guard band fits.
    float snr_db() const { return m_snr_db.load(); }
    // Raw ultrasonic-noise-ratio proxy (dB): what the squelch state machine
    // runs on. Pumps with program loudness -- not for display.
    float squelch_metric_db() const { return m_squelch_metric_db.load(); }
    // Effective K_noise in use (env / config / compiled default), for --snr-cal.
    float snr_knoise() const { return m_snr_knoise; }
    bool squelch_open() const { return m_sq_open_pub.load(); }
    float agc_gain_db() const { return m_agc_gain_db.load(); }
    // AFC correction currently folded into the fine mixer (Hz): the
    // estimated carrier-frequency error, sign such that a positive value
    // means the station sits above its nominal frequency. 0 while squelched
    // or in capture-only mode.
    float afc_hz() const { return m_afc_hz_pub.load(); }
    // Live high-cut corner (Hz). Equals the full audio bandwidth when the
    // signal is clean (high-cut inactive); drops toward ~4.5 kHz under noise.
    float high_cut_hz() const { return m_high_cut_hz.load(); }
    // Multipath indicator: normalised ripple of the pre-demod IF envelope as
    // a 0..100 "%" AM-depth figure -- ~0 for a clean constant-modulus
    // carrier, rising with frequency-selective multipath fading. Holds its
    // last value while squelched; 0 in capture-only mode.
    float multipath() const { return m_multipath_pub.load(); }
    // FM demod in use for this station: "discriminator" or "pll".
    // "pll" is forced back to "discriminator" for stereo/RDS stations in
    // resolve.hpp.
    const char* demod_mode() const { return m_demod_mode.c_str(); }
    // Impulsive-click suppression rate (parts-per-million of discriminator
    // samples replaced), EMA-smoothed. 0 when fm.declick is off.
    float declick_ppm() const { return m_declick_ppm_pub.load(); }
    // Pilot PLL state for a stereo station (false for mono): a genuine
    // phase-lock flag, and the tracked 19 kHz pilot frequency (Hz).
    bool pilot_lock() const {
        return m_stereo ? m_stereo->pilot_locked() : false;
    }
    float pilot_hz() const {
        return m_stereo ? m_stereo->pilot_freq_hz() : 0.0f;
    }

    // Listener-gated decode: the RT loop calls this instead of process()
    // for a station nobody is listening to. No DSP runs; the published
    // metrics simply stop updating until a listener returns.
    void mark_idle() { m_idle.store(true, std::memory_order_relaxed); }
    bool idle() const { return m_idle.load(std::memory_order_relaxed); }

    // stereo status (all inert for a mono station)
    bool stereo() const { return static_cast<bool>(m_stereo); }
    float stereo_frac() const {
        return m_stereo ? m_stereo->stereo_frac() : 0.0f;
    }
    float pilot_db() const {
        return m_stereo ? m_stereo->pilot_db() : -120.0f;
    }

    // RDS status (all inert if RDS not enabled or station not in active set)
    bool rds() const { return static_cast<bool>(m_rds); }
    RdsPublic rds_snapshot() const {
        return m_rds ? m_rds->snapshot() : RdsPublic();
    }

    // `bin` : this station's channelizer output, channel_rate complex
    // samples for one input block. Runs the whole chain and emits UDP.
    void process(const cfloat* bin, size_t n) {
        m_idle.store(false, std::memory_order_relaxed);
        m_mix.resize(n);
        nco_crcf_mix_block_down(m_nco, const_cast<cfloat*>(bin), m_mix.data(),
                                static_cast<unsigned int>(n));

        m_filtered.resize(n);
        firfilt_crcf_execute_block(m_predemod, m_mix.data(),
                                   static_cast<unsigned int>(n),
                                   m_filtered.data());

        // FM discriminator (+ de-emphasis for the mono path), one pass at
        // channel_rate. m_disc keeps the raw (pre de-emphasis) output --
        // the squelch metric and the stereo decoder both read it.
        m_disc.resize(n);
        if (!m_stereo)
            m_demod.resize(n);
        double disc_sum = 0.0;                 // block mean feeds AFC
        double mp_ac_pow = 0.0, mp_dc_sum = 0.0; // IF-envelope ripple feeds multipath
        for (size_t i = 0; i < n; ++i) {
            const cfloat cur = m_filtered[i];

            if (!m_capture_only) {
                // Multipath: normalised ripple of the pre-demod IF envelope.
                const float mag = std::sqrt(cur.real() * cur.real() +
                                            cur.imag() * cur.imag());
                m_mp_dc += m_mp_dc_a * (mag - m_mp_dc);
                const float ac = mag - m_mp_dc;
                mp_ac_pow += double(ac) * ac;
                mp_dc_sum += m_mp_dc;
            }

            // FM demod: PLL feedback loop (mono, opt-in) or the open-loop
            // 1-sample-delay atan2 discriminator. Both yield f_inst /
            // deviation via the same m_k scale, so m_disc semantics are
            // identical for every downstream consumer.
            float d;
            if (m_pll_demod) {
                d = static_cast<float>(m_k) * m_pll_demod->step(cur);
            } else {
                const cfloat prod = cur * std::conj(m_prev);
                m_prev = cur;
                d = static_cast<float>(m_k) *
                    std::atan2(prod.imag(), prod.real());
            }
            if (m_declicker)
                d = m_declicker->step(d);
            m_disc[i] = d;
            disc_sum += d;

            if (!m_stereo) {
                // y[n] = b0*(x[n] + x[n-1]) + p1*y[n-1], i.e. the one-pole
                // de-emphasis shelf H(z) = b0*(1 + z^-1) / (1 - p1*z^-1)
                // whose b0/p1 are derived from the tau in resolve.hpp.
                // Stereo does its own per-channel de-emphasis after the L/R
                // matrix.
                const float y = m_deemph_b0 * (d + m_deemph_x_prev) +
                                m_deemph_p1 * m_deemph_y_prev;
                m_deemph_x_prev = d;
                m_deemph_y_prev = y;
                m_demod[i] = y;
            }
        }

        // RDS decoder (if enabled and station is in active set).
        // Runs on the raw discriminator output before de-emphasis or stereo
        // processing. Only active when listener-gated decode includes this station.
        if (m_rds)
            m_rds->process(m_disc.data(), n);

        // Always run: the stereo blend and the high-cut read the calibrated
        // snr_db, the squelch state machine reads the proxy, and
        // --selftest / --sweep / --snr-cal read the calibrated value back.
        // The cost is ~3 IIR passes plus a couple of sums per block.
        update_metrics(bin, n);

        // AFC: integrate the discriminator DC mean into the fine mixer.
        // Skipped in capture-only mode (offline analysis must see the plan's
        // exact tuning) and while squelched (mean is just noise then).
        update_afc(disc_sum, n);

        // Multipath indicator: EMA the normalised IF-envelope ripple.
        // Skipped in capture-only mode; frozen while squelched (the envelope
        // of open-squelch noise is not multipath).
        update_multipath(mp_ac_pow, mp_dc_sum, n);

        // Declick rate: EMA of the per-block click count, in
        // parts-per-million of samples. ~0 on a clean signal, climbing
        // through the FM threshold knee.
        if (m_declicker && n > 0) {
            const unsigned long long c = m_declicker->clicks();
            const float blk_ppm =
                1e6f * static_cast<float>(c - m_declick_prev) /
                static_cast<float>(n);
            m_declick_prev = c;
            const float a = 1.0f - std::exp(-static_cast<float>(n) /
                                            (m_channel_rate * 0.5f));
            m_declick_ppm_ema += a * (blk_ppm - m_declick_ppm_ema);
            m_declick_ppm_pub.store(m_declick_ppm_ema);
        }

        if (!m_stereo) {
            if (const char* dbg = std::getenv("FMRX_DEBUG")) {
                (void)dbg;
                double bs = 0, ds = 0, bpk = 0, dpk = 0;
                for (size_t i = 0; i < n; ++i) {
                    double bm = std::abs(m_filtered[i]);
                    bs += bm * bm;
                    bpk = std::max(bpk, bm);
                    ds += double(m_demod[i]) * m_demod[i];
                    dpk = std::max(dpk, std::fabs((double)m_demod[i]));
                }
                std::fprintf(
                    stderr,
                    "[%s] |bin| rms=%.4f pk=%.4f  demod rms=%.4f pk=%.4f\n",
                    m_label.c_str(), std::sqrt(bs / n), bpk, std::sqrt(ds / n),
                    dpk);
            }
        }

        if (m_stereo) {
            emit_stereo();
            return;
        }

        m_audio_a.clear();
        m_stage_a.process(m_demod.data(), m_demod.size(), m_audio_a);
        m_audio_b.clear();
        m_stage_b.process(m_audio_a.data(), m_audio_a.size(), m_audio_b);

        if (m_audio_b.empty())
            return;

        if (!m_capture_only && m_agc_enabled)
            apply_agc();

        // Mono path: a station that emits stereo frames always has an
        // m_stereo decoder and returned via emit_stereo() above, so this
        // is a plain single-channel int16 stream.
        const size_t nframe = m_audio_b.size();
        m_pcm.resize(nframe);
        if (m_capture_only) {
            // Offline analysis path: no high-cut / soft-mute / squelch --
            // level and gain-envelope shaping is not DSP correctness.
            for (size_t i = 0; i < nframe; ++i)
                m_pcm[i] = f2s(m_audio_b[i]);
        } else {
            update_high_cut();                                     // high-cut
            const float mute_target =
                (m_sq_enabled && !m_sq_open) ? 0.0f : 1.0f;        // mute
            for (size_t i = 0; i < nframe; ++i) {
                float s = high_cut_step(m_audio_b[i]);             // high-cut
                s = m_pilot_notch.step(s);                         // pilot notch
                s *= mute_step(mute_target);                       // mute
                m_pcm[i] = f2s(m_limiter_enabled ? m_limiter.step1(s) : s);
            }
        }
        if (m_capture_only)
            captured.insert(captured.end(), m_pcm.begin(), m_pcm.end());
        else
            m_sink.send_pcm(m_pcm.data(), m_pcm.size());
    }

    // populated only when constructed with capture_only = true
    std::vector<int16_t> captured;

private:
    static int16_t f2s(float x) {
        float v = std::rint(x * 32767.0f);
        v = std::max(-32768.0f, std::min(32767.0f, v));
        return static_cast<int16_t>(v);
    }

    // Stereo path: discriminator -> StereoDecoder -> AGC -> squelch mute ->
    // interleaved int16 -> UDP. Reached whenever m_stereo is set; in
    // capture_only mode (--stereo-selftest) the AGC / squelch steps are
    // skipped and the frames land in `captured` instead of the UDP sink.
    void emit_stereo() {
        m_audio_L.clear();
        m_audio_R.clear();
        // The blend runs off the calibrated, program-independent snr_db
        // (same scalar that drives the high-cut below), not the loudness-
        // pumping ultrasonic-noise proxy. stereo_blend_snr_lo/hi_db are read
        // on that scale. The multipath % is a second blend axis -- a
        // clean-C/N but multipath-limited station still blends toward mono.
        m_stereo->process(m_disc.data(), m_disc.size(), m_snr_db.load(),
                          m_multipath_pub.load(), m_audio_L, m_audio_R);
        if (m_audio_L.empty())
            return;

        if (!m_capture_only && m_agc_enabled)
            apply_agc_stereo();

        const size_t nframe = m_audio_L.size();
        m_pcm.resize(nframe * 2);
        if (m_capture_only) {
            for (size_t i = 0; i < nframe; ++i) {
                m_pcm[2 * i] = f2s(m_audio_L[i]);
                m_pcm[2 * i + 1] = f2s(m_audio_R[i]);
            }
        } else {
            update_high_cut();                                     // high-cut
            const float mute_target =
                (m_sq_enabled && !m_sq_open) ? 0.0f : 1.0f;        // mute
            for (size_t i = 0; i < nframe; ++i) {
                const float g = mute_step(mute_target);            // mute
                const float l =
                    m_pilot_notch.step(high_cut_step(m_audio_L[i])) * g;
                const float r =
                    m_pilot_notch_r.step(high_cut_step_r(m_audio_R[i])) * g;
                float lo = l, ro = r;
                if (m_limiter_enabled)
                    m_limiter.step2(l, r, lo, ro);   // pilot notch/high-cut in, limiter out
                m_pcm[2 * i] = f2s(lo);
                m_pcm[2 * i + 1] = f2s(ro);
            }
        }
        if (m_capture_only)
            captured.insert(captured.end(), m_pcm.begin(), m_pcm.end());
        else
            m_sink.send_pcm(m_pcm.data(), m_pcm.size());
    }

    // AGC for the stereo path: track the level off the (L+R) mono sum so
    // the two channels always get the identical gain (no image shift).
    // Applies linear gain only -- peak control is the look-ahead limiter
    // at the tail of emit_stereo(), not a per-sample knee here.
    void apply_agc_stereo() {
        for (size_t i = 0; i < m_audio_L.size(); ++i) {
            const float mono = 0.5f * (m_audio_L[i] + m_audio_R[i]);
            m_agc_rms2 += m_agc_rms_alpha * (mono * mono - m_agc_rms2);
            const float rms = std::sqrt(m_agc_rms2) + 1e-6f;
            const float want = std::min(m_agc_target / rms, m_agc_max_gain);
            const float ga = (want < m_agc_gain) ? m_agc_attack_alpha
                                                 : m_agc_release_alpha;
            m_agc_gain += ga * (want - m_agc_gain);
            if (m_limiter_enabled) {
                m_audio_L[i] *= m_agc_gain;
                m_audio_R[i] *= m_agc_gain;
            } else { // limiter off -> the in-AGC tanh soft-clip
                m_audio_L[i] = soft_clip(m_audio_L[i] * m_agc_gain);
                m_audio_R[i] = soft_clip(m_audio_R[i] * m_agc_gain);
            }
        }
        m_agc_gain_db.store(20.0f * std::log10(std::max(m_agc_gain, 1e-6f)));
    }

    // Linear gain only; the look-ahead limiter at the tail of
    // process() is the peak stage.
    void apply_agc() {
        for (float& xr : m_audio_b) {
            const float x = xr;
            m_agc_rms2 += m_agc_rms_alpha * (x * x - m_agc_rms2);
            const float rms = std::sqrt(m_agc_rms2) + 1e-6f;
            const float want = std::min(m_agc_target / rms, m_agc_max_gain);
            const float ga =
                (want < m_agc_gain) ? m_agc_attack_alpha : m_agc_release_alpha;
            m_agc_gain += ga * (want - m_agc_gain);
            xr = m_limiter_enabled ? x * m_agc_gain
                                   : soft_clip(x * m_agc_gain); // limiter off
        }
        m_agc_gain_db.store(20.0f * std::log10(std::max(m_agc_gain, 1e-6f)));
    }

    // --- soft-mute --------------------------------------------------
    // Advance the one-pole gain envelope one audio sample toward `target`
    // (1 = pass, 0 = muted) and return the new gain. Asymmetric: quicker to
    // mute than to un-mute is fine, but here open is the fast edge so a
    // returning signal isn't clipped off the front.
    float mute_step(float target) {
        const float a = (target > m_mute_gain) ? m_mute_attack_a
                                               : m_mute_release_a;
        m_mute_gain += a * (target - m_mute_gain);
        return m_mute_gain;
    }

    // --- progressive high-cut -----------------------------------------
    // Once per block: map the calibrated snr_db to a target LPF corner,
    // slew-limit it, and precompute the one-pole coefficient the per-sample
    // steps below use. During the startup warmup the corner is pinned wide
    // open (snr_db hasn't settled yet).
    void update_high_cut() {
        float target = m_hc_max_hz;
        if (m_warmup_samp <= 0) {
            const float snr = m_snr_db.load();
            float t = (snr - kHiCutLoSnrDb) / (kHiCutHiSnrDb - kHiCutLoSnrDb);
            t = std::clamp(t, 0.0f, 1.0f);
            target = kHiCutMinHz + t * (m_hc_max_hz - kHiCutMinHz);
        }
        m_hc_corner += kHiCutCornerSlew * (target - m_hc_corner);
        // Report the corner clamped to the audio band -- above that the
        // filter is doing nothing the audio stages didn't already do.
        m_high_cut_hz.store(std::min(m_hc_corner, m_hc_audio_bw));
        m_hc_a = (m_hc_corner >= m_hc_max_hz - 100.0f)
                     ? 1.0f // effectively transparent -> pass-through
                     : 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) *
                                       m_hc_corner / m_hc_fs);
    }
    float high_cut_step(float x) {
        m_hc_y += m_hc_a * (x - m_hc_y);
        return m_hc_y;
    }
    float high_cut_step_r(float x) {
        m_hc_yr += m_hc_a * (x - m_hc_yr);
        return m_hc_yr;
    }

    // --- AFC ----------------------------------------------------------
    void update_afc(double disc_sum, size_t n) {
        if (!m_afc_enabled) // `fm.afc: false` / FMRX_NO_AFC -> static mixer
            return;
        if (m_capture_only)
            return;
        if (m_warmup_samp > 0) {
            m_warmup_samp -= static_cast<long long>(n);
            return;
        }
        if (!m_sq_open) // frozen while squelched -- the mean is only noise
            return;
        const float err_hz =
            static_cast<float>(disc_sum / n) * m_afc_dev_hz;
        m_afc_hz = std::clamp(m_afc_hz + kAfcMu * err_hz, -kAfcRangeHz,
                              kAfcRangeHz);
        nco_crcf_set_frequency(
            m_nco, static_cast<float>(2.0 * M_PI * (m_fine_hz + m_afc_hz) /
                                      m_afc_ch_rate));
        m_afc_hz_pub.store(m_afc_hz);
    }

    // --- multipath indicator -----------------------------------------
    // ac_pow / dc_sum are this block's accumulated IF-envelope AC power and
    // slow-mean sum from the discriminator loop. Publish the EMA-smoothed
    // ratio (~300 ms, matching the guard-band SNR) as a 0..100 "%" figure.
    void update_multipath(double ac_pow, double dc_sum, size_t n) {
        if (m_capture_only || n == 0)
            return;
        if (m_sq_enabled && !m_sq_open) // envelope of open-squelch noise != multipath
            return;
        const float ac_rms = std::sqrt(static_cast<float>(ac_pow / n));
        const float dc_mean = static_cast<float>(dc_sum / n) + 1e-9f;
        const float depth = ac_rms / dc_mean;
        const float a =
            1.0f - std::exp(-static_cast<float>(n) / (m_channel_rate * 0.3f));
        m_mp_ema += a * (depth - m_mp_ema);
        m_multipath_pub.store(100.0f * m_mp_ema);
    }

    void update_metrics(const cfloat* bin, size_t n) {
        // RF level: RMS of this station's channelizer bin, in dBFS.
        double bp = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const float re = bin[i].real(), im = bin[i].imag();
            bp += double(re) * re + double(im) * im;
        }
        const double brms = std::sqrt(bp / n);
        m_rf_dbfs.store(static_cast<float>(20.0 * std::log10(std::max(brms, 1e-9))));

        // --- raw ultrasonic-noise-ratio proxy (drives the squelch state
        // machine only; pumps with modulation, never displayed) ---
        double noise_pow = 0.0, total_pow = 0.0;
        for (size_t i = 0; i < n; ++i) {
            float hp;
            iirfilt_rrrf_execute(m_hpf, m_disc[i], &hp);
            noise_pow += double(hp) * hp;
            total_pow += double(m_disc[i]) * m_disc[i];
        }
        const double ratio = noise_pow / std::max(total_pow, 1e-12);
        const float proxy =
            static_cast<float>(-10.0 * std::log10(std::max(ratio, 1e-9)));
        m_squelch_metric_db.store(proxy);

        // --- calibrated, program-independent SNR (see ctor) ---
        // Always computed -- snr_db has no proxy fallback; the guard band
        // is guaranteed placeable by resolve_config.
        {
            double gpow = 0.0;
            for (size_t i = 0; i < n; ++i) {
                float hp, g;
                iirfilt_rrrf_execute(m_gb_hp, m_disc[i], &hp);
                iirfilt_rrrf_execute(m_gb_lp, hp, &g);
                gpow += double(g) * g;
            }
            const float pg_blk = static_cast<float>(gpow / n);
            // EMA-smooth the guard-band power estimate (~300 ms) -- the
            // ~13 kHz band jitters several dB block to block on its own.
            const float alpha =
                1.0f - std::exp(-static_cast<float>(n) / (m_channel_rate * 0.3f));
            m_pg_ema = (m_pg_ema <= 0.0f) ? pg_blk
                                          : m_pg_ema + alpha * (pg_blk - m_pg_ema);
            const double N0 = double(m_pg_ema) * m_inv_guard_integral;
            const double p_noise =
                N0 * m_inband_deemph_integral * double(m_snr_knoise);
            m_snr_db.store(static_cast<float>(
                10.0 * std::log10(m_snr_pref / std::max(p_noise, 1e-30))));
        }

        // The state machine (and so `squelch_open`) only means anything when
        // squelch is enabled; it runs on the raw proxy, not the calibrated
        // value.
        if (!m_sq_enabled) {
            m_sq_open_pub.store(true);
            return;
        }
        if (proxy >= m_sq_open_snr_db) {
            m_sq_open = true;
            m_sq_hang_left = m_sq_hang_samp;
        } else if (proxy < m_sq_open_snr_db - 3.0f) { // hysteresis
            if (m_sq_hang_left > 0)
                m_sq_hang_left -= static_cast<long long>(n);
            else
                m_sq_open = false;
        }
        m_sq_open_pub.store(m_sq_open);
    }

    std::string m_label;
    bool m_capture_only = false;
    double m_k;
    float m_deemph_b0, m_deemph_p1;
    float m_deemph_x_prev = 0.0f, m_deemph_y_prev = 0.0f;
    cfloat m_prev{0.0f, 0.0f};

    // --- weak-signal demod ---
    static constexpr float kPllDemodBwHz = 45000.0f; // PLL-demod loop BW
    static constexpr float kDeclickFloor = 0.12f;    // min sigma*scale to act
    std::string m_demod_mode = "discriminator";
    std::unique_ptr<PllFmDemod> m_pll_demod;   // set only for fm.demod: pll
    std::unique_ptr<Declicker> m_declicker;    // set only for fm.declick: true
    unsigned long long m_declick_prev = 0;
    float m_declick_ppm_ema = 0.0f;

    nco_crcf m_nco = nullptr;
    firfilt_crcf m_predemod = nullptr;
    RealLpfDecimator m_stage_a, m_stage_b;
    UdpSink m_sink;

    // soft AGC
    bool m_agc_enabled = true;
    float m_agc_target = 0.5f, m_agc_max_gain = 100.0f;
    float m_agc_rms_alpha = 0.0f, m_agc_attack_alpha = 0.0f,
          m_agc_release_alpha = 0.0f;
    float m_agc_rms2 = 0.25f, m_agc_gain = 1.0f;

    // squelch
    bool m_sq_enabled = false;
    float m_sq_open_snr_db = 20.0f;
    long long m_sq_hang_samp = 0, m_sq_hang_left = 0;
    bool m_sq_open = true;
    iirfilt_rrrf m_hpf = nullptr;

    // Soft-mute ramp: one-pole gain envelope on the final audio.
    float m_mute_gain = 0.0f; // starts muted -> first audio fades in
    float m_mute_attack_a = 0.0f, m_mute_release_a = 0.0f;

    // Progressive high-cut: variable one-pole LPF on the final audio,
    // corner driven by the calibrated snr_db, slew-limited block to block.
    // snr_db ~= recovered audio SNR: hissy in the 30s, clean by ~52.
    // Corner floors on the worst fringe stations and is wide open for
    // anything comfortably listenable.
    static constexpr float kHiCutLoSnrDb = 30.0f; // corner floored at/below
    static constexpr float kHiCutHiSnrDb = 52.0f; // corner wide open at/above
    static constexpr float kHiCutMinHz = 4500.0f;
    static constexpr float kHiCutCornerSlew = 0.08f; // per block (~100 ms)
    float m_hc_audio_bw = 15000.0f; // full passband -> "high-cut inactive"
    float m_hc_fs = 48000.0f;       // audio_rate
    float m_hc_max_hz = 21600.0f;   // corner at/above which we pass through
    float m_hc_corner = 21600.0f;   // slewed corner, Hz
    float m_hc_a = 1.0f;            // one-pole coeff for the current corner
    float m_hc_y = 0.0f, m_hc_yr = 0.0f; // filter state (mono/L, R)

    // 19 kHz pilot notch on the final audio (own state for mono/L and R;
    // both inert at an audio rate that puts 19 kHz above ~0.47*fs).
    static constexpr float kPilotNotchHz = 19000.0f;
    static constexpr float kPilotNotchQ = 5.0f; // ~3.8 kHz -3 dB width
    BiquadNotch m_pilot_notch, m_pilot_notch_r;

    // Look-ahead brickwall limiter -- the final peak stage before int16.
    static constexpr float kLimLookaheadMs = 1.5f;
    static constexpr float kLimReleaseMs = 60.0f;
    static constexpr float kLimCeiling = 0.985f; // leave a hair below full scale
    LookaheadLimiter m_limiter;
    bool m_limiter_enabled = true; // `fm.limiter` / FMRX_NO_LIMITER

    // Multipath indicator: EMA of the normalised pre-demod IF-envelope
    // ripple. m_mp_dc is a slow one-pole envelope mean (corner set by
    // kMultipathDcTauMs); ripple faster than that reads as multipath.
    static constexpr float kMultipathDcTauMs = 20.0f;
    float m_mp_dc = 0.0f, m_mp_ema = 0.0f, m_mp_dc_a = 0.0f;

    // AFC: slow integrator of the discriminator DC mean into m_nco.
    // mu 0.1/block is a ~10-block (few-second) first-order pull-in with no
    // overshoot; the block-to-block disc-mean noise it passes through is
    // sub-Hz. Drift being tracked (TCXO temp) is minutes-scale.
    static constexpr float kAfcMu = 0.1f;         // integrator gain per block
    static constexpr float kAfcRangeHz = 4000.0f; // correction clamp
    bool m_afc_enabled = true;   // `fm.afc` / FMRX_NO_AFC
    double m_fine_hz = 0.0;      // configured fine-mixer shift (Hz)
    float m_afc_dev_hz = 0.0f;   // fm_deviation_hz, scales the disc mean
    float m_afc_ch_rate = 0.0f;
    float m_afc_hz = 0.0f;       // current correction (Hz)
    long long m_warmup_samp = 0; // AFC + high-cut frozen until this hits 0

    // calibrated SNR readout. K_noise absorbs pre-demod passband ripple, the
    // discriminator's exact noise bandwidth, the 15-19 kHz audio-LPF skirt
    // and windowing -- everything cheaper to measure than model. Fit it with
    //   feedmyfm-rx -c config.yml -s stations.yml --snr-cal
    // on the 16.384 MHz / 64-ch baseline (config.yml), 76-88 kHz guard band,
    // over the 9-33 dB C/N region; override at runtime with FMRX_SNR_KNOISE.
    // If the
    // re-run --snr-cal and either re-bake this or -- to skip a rebuild --
    // set `snr.k_noise` in config.yml (or the FMRX_SNR_KNOISE env).
    static constexpr float kSnrKNoiseDefault = 2.278f;
    float m_channel_rate = 0.0f;
    double m_inv_guard_integral = 0.0;
    double m_inband_deemph_integral = 0.0;
    double m_snr_pref = 0.5;
    float m_snr_knoise = kSnrKNoiseDefault;
    float m_pg_ema = 0.0f;
    iirfilt_rrrf m_gb_hp = nullptr, m_gb_lp = nullptr; // guard-band HP+LP

    // stereo multiplex decoder -- null unless stereo_mode != Mono. When
    // set, the mono de-emphasis / audio stages are skipped and
    // emit_stereo() takes over.
    std::unique_ptr<StereoDecoder> m_stereo;

    // RDS (Radio Data System) decoder -- null unless plan.rds_enabled and
    // channel_rate >= 120 kHz. When set, processes the raw discriminator
    // m_disc to extract PI / PS / RadioText / PTY / CT / TP/TA.
    // Only active when station is in the listener-gated active set.
    std::unique_ptr<RdsDecoder> m_rds;

    // live metadata
    std::atomic<float> m_rf_dbfs{-120.0f};
    std::atomic<float> m_snr_db{0.0f};           // calibrated (status port)
    std::atomic<float> m_squelch_metric_db{0.0f}; // raw proxy (squelch + blend)
    std::atomic<float> m_agc_gain_db{0.0f};
    std::atomic<float> m_afc_hz_pub{0.0f};          // AFC tuning error (Hz)
    std::atomic<float> m_high_cut_hz{15000.0f};     // high-cut live LPF corner (Hz)
    std::atomic<float> m_multipath_pub{0.0f};       // multipath IF-envelope ripple (%)
    std::atomic<float> m_declick_ppm_pub{0.0f};     // click-suppression rate (ppm)
    std::atomic<bool> m_sq_open_pub{true};
    // Set by the RT loop when listener-gated decode skips this station for
    // a block (decode.listener_gated); cleared on the next real process().
    // Only tells the status port to mark the station idle -- every metric
    // above just holds its last live value while idle.
    std::atomic<bool> m_idle{false};

    // scratch reused across blocks
    std::vector<cfloat> m_mix, m_filtered;
    std::vector<float> m_demod, m_disc, m_audio_a, m_audio_b;
    std::vector<float> m_audio_L, m_audio_R; // stereo path
    std::vector<int16_t> m_pcm;
};

} // namespace fmrx
