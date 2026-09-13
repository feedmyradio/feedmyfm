// One FM station's stereo multiplex decoder.
//
//   raw discriminator output d[n] @ channel_rate (real, PRE de-emphasis;
//   this is MonoStation::m_disc)
//     -> composite LPF + decimate   channel_rate -> mpx_rate  (keep 0..58 kHz:
//                                     19 kHz pilot + 23..53 kHz L-R + 57 kHz RDS)
//     -> pilot band-pass @ 19 kHz    (complex FIR, analytic pilot)
//     -> regenerate 38 kHz carrier   nco_crcf PLL locked to the pilot,
//                                    phase doubled
//     -> L-R  = 2 * LPF( composite * carrier )      (coherent DSB-SC demod)
//     -> L+R  =     LPF( composite )
//     -> decimate both  mpx_rate -> audio_rate
//     -> blend factor `frac` (0=mono .. 1=full stereo), from pilot lock +
//        composite SNR:   L = (L+R) + frac*(L-R),  R = (L+R) - frac*(L-R)
//     -> per-channel de-emphasis IIR (own L / R state, coeffs at audio_rate)
//
// The pilot carrier is recovered by a narrow nco_crcf PLL locked to the
// 19 kHz pilot, then phase-doubled to 38 kHz -- better separation and a
// real (phase-coherence) lock flag at low C/N than the alternative
// normalise-and-square regen (`stereo_pilot_pll: false`), which doubles
// the pilot noise and needs a hand polarity knob. Filter taps come from
// liquid's Kaiser designer (see
// dsp_util.hpp); verify by ear / spectrum.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <liquid/liquid.h>

#include "dsp_util.hpp"
#include "plan.hpp"

namespace fmrx {

class StereoDecoder {
public:
    explicit StereoDecoder(const StationPlan& sp)
        : m_blend(sp.stereo_blend),
          m_use_pll(sp.stereo_pilot_pll),
          m_pilot_thr_db(static_cast<float>(sp.stereo_pilot_threshold_db)),
          m_snr_lo(static_cast<float>(sp.stereo_blend_snr_lo_db)),
          m_snr_hi(static_cast<float>(sp.stereo_blend_snr_hi_db)) {
        const int ch_rate = sp.channel_rate_hz;
        const int audio_rate = sp.audio_rate_hz;

        // channel_rate / audio_rate splits into (composite decim D) *
        // (audio decim). Pick the largest D that keeps the intermediate
        // MPX rate >= 120 kHz, so the pilot BPF / carrier / audio LPFs run
        // as cheaply as the pilot + L-R bandwidth allows. resolve.hpp has
        // already guaranteed channel_rate >= 120 kHz for a stereo station.
        const int total = ch_rate / audio_rate; // integer (see resolve.hpp)
        int D = 1;
        for (int d = 1; d <= total; ++d)
            if (total % d == 0 && ch_rate / d >= 120000)
                D = d;
        m_mpx_rate = ch_rate / D;
        const int audio_decim = total / D;

        // --- composite LPF + decimate: channel_rate -> mpx_rate ---
        // Pass the whole used MPX (pilot + L-R + RDS), stop by the new
        // Nyquist. Runs at channel_rate -- the one expensive filter here.
        {
            const double stop =
                std::min(m_mpx_rate / 2.0 - 1000.0, 63000.0);
            m_downsamp = RealLpfDecimator(ch_rate, 58000.0,
                                          std::max(stop, 58000.0 + 2000.0),
                                          60.0f, D);
        }

        // --- pilot band-pass @ 19 kHz (complex output) ---
        // Real Kaiser LPF prototype (+/- ~1 kHz), frequency-shifted up to
        // +19 kHz so a real input yields the analytic +19 kHz component.
        {
            std::vector<float> lp =
                kaiser_lpf_unity(m_mpx_rate, 1000.0, 3000.0, 50.0f);
            std::vector<cfloat> bp(lp.size());
            const double w = 2.0 * M_PI * 19000.0 / m_mpx_rate;
            for (size_t i = 0; i < lp.size(); ++i)
                bp[i] = lp[i] * cfloat(static_cast<float>(std::cos(w * i)),
                                       static_cast<float>(std::sin(w * i)));
            m_pilot_bpf = firfilt_cccf_create(
                bp.data(), static_cast<unsigned int>(bp.size()));
        }
        // No composite delay to compensate: because the LPF prototype has
        // unity DC gain (sum(h) == 1) and rejects 38 kHz, this
        // heterodyne-and-lowpass structure tracks the pilot's *phase*
        // instantaneously -- its group delay applies only to the amplitude
        // estimate. So carrier[n] is already aligned to composite[n].

        // --- 19 kHz pilot PLL ---
        // Locks a narrow nco_crcf to the analytic pilot above and phase-
        // doubles it to a clean 38 kHz. kStereoCarrierPhase (pi) reproduces
        // the polarity the old normalise-and-square used by default
        // (carrier = -sin(2*arg(pilot))), verified by --stereo-selftest;
        // it is a compiled constant now, not the FMRX_STEREO_INVERT knob.
        m_pilot_pll = std::make_unique<PilotPll>(
            m_mpx_rate, 19000.0, kPilotPllBwHz, kStereoCarrierPhase);

        // --- audio LPFs: mpx_rate -> audio_rate (L+R and L-R paths) ---
        m_lpr_lpf = RealLpfDecimator(m_mpx_rate, sp.fm_audio_bw_hz,
                                     sp.fm_audio_stop_hz, 60.0f, audio_decim);
        m_lmr_lpf = RealLpfDecimator(m_mpx_rate, sp.fm_audio_bw_hz,
                                     sp.fm_audio_stop_hz, 60.0f, audio_decim);

        // --- per-channel de-emphasis (bilinear transform at audio_rate) ---
        // The stereo path de-emphasises AFTER its audio decimate stage,
        // unlike the mono demod (before) -- so coeffs are evaluated at
        // audio_rate, not channel_rate.
        {
            const double fs = audio_rate;
            const double w_c = 1.0 / sp.fm_tau_s;
            const double w_ca = 2.0 * fs * std::tan(w_c / (2.0 * fs));
            const double k = -w_ca / (2.0 * fs);
            m_deemph_p1 = static_cast<float>((1.0 + k) / (1.0 - k));
            m_deemph_b0 = static_cast<float>(-k / (1.0 - k));
        }
    }

    StereoDecoder(const StereoDecoder&) = delete;
    StereoDecoder& operator=(const StereoDecoder&) = delete;
    ~StereoDecoder() {
        if (m_pilot_bpf)
            firfilt_cccf_destroy(m_pilot_bpf);
    }

    int mpx_rate() const { return m_mpx_rate; }
    float pilot_db() const { return m_pilot_db; }
    float stereo_frac() const { return m_frac; }
    bool pilot_locked() const { return m_locked; }
    // Tracked 19 kHz pilot frequency (Hz) from the PLL; 0 if never built.
    float pilot_freq_hz() const {
        return m_pilot_pll ? m_pilot_pll->freq_hz() : 0.0f;
    }

    // `disc` : raw discriminator output, `n` real samples at channel_rate.
    // `snr_db` : the station's live composite SNR (MonoStation's metric).
    // `multipath_pct` : IF-envelope ripple 0..100. In `auto` mode a
    //   high value blends L/R down regardless of SNR -- a clean-C/N but
    //   multipath-limited signal still sounds bad in full stereo.
    // Appends `n / (channel_rate/audio_rate)` samples to each of outL/outR.
    void process(const float* disc, size_t n, float snr_db, float multipath_pct,
                 std::vector<float>& outL, std::vector<float>& outR) {
        // 1. composite -> mpx_rate
        m_comp.clear();
        m_downsamp.process(disc, n, m_comp);
        const size_t m = m_comp.size();
        if (m == 0)
            return;

        // 2. pilot BPF (complex) + 3. regenerate the 38 kHz carrier, and
        //    4. build the L+R (composite) and L-R (composite x carrier) inputs.
        m_lpr_in.resize(m);
        m_lmr_in.resize(m);
        double pilot_pow = 0.0, comp_pow = 0.0;
        for (size_t i = 0; i < m; ++i) {
            const float c = m_comp[i];
            comp_pow += double(c) * c;

            cfloat pout;
            firfilt_cccf_push(m_pilot_bpf, cfloat(c, 0.0f));
            firfilt_cccf_execute(m_pilot_bpf, &pout);
            const cfloat p = pout;
            pilot_pow += double(std::norm(p));

            // 38 kHz subcarrier: PLL-locked (phase-doubled) by default, or
            // the old normalise-and-square when `stereo.pilot_pll: false`.
            const float carrier = m_use_pll ? m_pilot_pll->step(p)
                                            : m_pilot_pll->carrier_square(p);

            m_lpr_in[i] = c;
            m_lmr_in[i] = 2.0f * c * carrier; // coherent DSB-SC demod (x2)
        }

        // pilot presence: EMA of pilot power vs composite power, in dBc.
        const double pilot_mean = pilot_pow / m;
        const double comp_mean = comp_pow / m + 1e-20;
        m_pilot_pow_ema += m_pow_alpha * (pilot_mean - m_pilot_pow_ema);
        m_comp_pow_ema += m_pow_alpha * (comp_mean - m_comp_pow_ema);
        m_pilot_db = static_cast<float>(
            10.0 * std::log10(std::max(m_pilot_pow_ema, 1e-30) /
                              std::max(m_comp_pow_ema, 1e-30)));
        // Lock needs both the pilot-band power (dBc, 3 dB hysteresis) and
        // the PLL phase-error coherence, so a strong adjacent carrier or a
        // broadband noise bump in the pilot band doesn't read as "stereo"
        // on power alone. In square mode there is no coherence estimate,
        // so lock is power-only.
        const float coh = m_use_pll ? m_pilot_pll->coherence() : 1.0f;
        if (m_pilot_db > m_pilot_thr_db && coh > kPilotLockCoh)
            m_locked = true;
        else if (m_pilot_db < m_pilot_thr_db - 3.0f || coh < kPilotUnlockCoh)
            m_locked = false;

        // blend target: pilot gates it; in `auto` the depth is the MIN of
        // an SNR window and a multipath window -- either a noisy L-R
        // (low SNR) or a frequency-selective fade (high multipath %) pulls
        // it toward mono. In forced stereo it's full whenever pilot-locked.
        float target = 0.0f;
        if (m_locked) {
            if (!m_blend) {
                target = 1.0f;
            } else {
                const float snr_t = std::clamp(
                    (snr_db - m_snr_lo) / (m_snr_hi - m_snr_lo), 0.0f, 1.0f);
                const float mp_t = std::clamp(
                    (kBlendMpHi - multipath_pct) / (kBlendMpHi - kBlendMpLo),
                    0.0f, 1.0f);
                target = std::min(snr_t, mp_t);
            }
        }
        m_frac += m_frac_alpha * (target - m_frac);

        // 5. audio LPF + decimate both paths
        m_lpr.clear();
        m_lmr.clear();
        m_lpr_lpf.process(m_lpr_in.data(), m_lpr_in.size(), m_lpr);
        m_lmr_lpf.process(m_lmr_in.data(), m_lmr_in.size(), m_lmr);
        const size_t nf = std::min(m_lpr.size(), m_lmr.size());

        // 6. matrix + per-channel de-emphasis
        for (size_t i = 0; i < nf; ++i) {
            const float lpr = m_lpr[i];
            const float lmr = m_frac * m_lmr[i];
            float l = lpr + lmr;
            float r = lpr - lmr;

            const float yl = m_deemph_b0 * (l + m_deemph_xl) + m_deemph_p1 * m_deemph_yl;
            m_deemph_xl = l;
            m_deemph_yl = yl;
            const float yr = m_deemph_b0 * (r + m_deemph_xr) + m_deemph_p1 * m_deemph_yr;
            m_deemph_xr = r;
            m_deemph_yr = yr;

            outL.push_back(yl);
            outR.push_back(yr);
        }
    }

private:
    // 19 kHz pilot PLL loop bandwidth (Hz) -- narrow: the pilot is an
    // unmodulated carrier and a tight loop is what rejects noise at the C/N
    // where `auto` still wants stereo.
    static constexpr float kPilotPllBwHz = 80.0f;
    // Fixed carrier polarity/phase, folded from the discriminator sign and
    // the phase-instantaneous pilot BPF (pi == the old default). Verified
    // by --stereo-selftest -- retune only if the BPF design or the
    // discriminator convention changes.
    static constexpr float kStereoCarrierPhase = 3.14159265358979f;
    static constexpr float kPilotLockCoh = 0.85f;   // enter lock above this
    static constexpr float kPilotUnlockCoh = 0.60f; // drop lock below this
    // `auto` multipath blend window (%): no stereo penalty below Lo,
    // forced full-mono at/above Hi. A clean-C/N signal with frequency-
    // selective fading still sounds bad in full stereo.
    static constexpr float kBlendMpLo = 25.0f;
    static constexpr float kBlendMpHi = 55.0f;

    bool m_blend;
    bool m_use_pll; // stereo.pilot_pll -- false = normalise-and-square
    float m_pilot_thr_db, m_snr_lo, m_snr_hi;

    int m_mpx_rate = 0;
    RealLpfDecimator m_downsamp;
    firfilt_cccf m_pilot_bpf = nullptr;
    std::unique_ptr<PilotPll> m_pilot_pll;

    RealLpfDecimator m_lpr_lpf, m_lmr_lpf;

    float m_deemph_b0 = 0.0f, m_deemph_p1 = 0.0f;
    float m_deemph_xl = 0.0f, m_deemph_yl = 0.0f;
    float m_deemph_xr = 0.0f, m_deemph_yr = 0.0f;

    // pilot / blend state. Alphas are per process()-block (~8 ms blocks in
    // the live loop): ~50 ms power averaging, ~50 ms blend slew.
    double m_pilot_pow_ema = 0.0, m_comp_pow_ema = 0.0;
    static constexpr float m_pow_alpha = 0.15f;
    static constexpr float m_frac_alpha = 0.15f;
    float m_pilot_db = -120.0f;
    bool m_locked = false;
    float m_frac = 0.0f;

    // scratch reused across blocks
    std::vector<float> m_comp, m_lpr_in, m_lmr_in, m_lpr, m_lmr;
};

} // namespace fmrx
