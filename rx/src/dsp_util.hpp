// Small DSP helpers shared by the mono and stereo per-station chains
// (mono_station.hpp, stereo_decoder.hpp). Nothing here does any planning --
// every rate / band edge is passed in from the resolved StationPlan.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

#include <liquid/liquid.h>

namespace fmrx {

using cfloat = std::complex<float>;

// One-pole smoothing coefficient for a `ms` millisecond time constant at
// sample rate `fs`.
inline float alpha_from_ms(double ms, double fs) {
    return static_cast<float>(1.0 - std::exp(-1000.0 / (std::max(ms, 0.1) * fs)));
}

// Memoryless tanh soft-clip knee above +/-0.8. The peak stage before the
// look-ahead limiter; kept as the `fm.limiter: false` fallback so the
// limiter can be A/B'd from config.
inline float soft_clip(float x) {
    const float t = 0.8f;
    if (x > t)
        return t + (1.0f - t) * std::tanh((x - t) / (1.0f - t));
    if (x < -t)
        return -t + (1.0f - t) * std::tanh((x + t) / (1.0f - t));
    return x;
}

// Kaiser-window low-pass prototype, normalised to unity DC gain: flat
// passband out to `passband_hz`, then a transition reaching the stopband
// at `stopband_hz`.
//
// (liquid's liquid_firdes_kaiser takes fc = the -6 dB point and does NOT
// normalise; passing the passband edge as fc there -- an earlier bug --
// left the response 6 dB down at the top of the audio band and made
// everything sound dull. Here fc is the transition midpoint.)
inline std::vector<float> kaiser_lpf_unity(double fs, double passband_hz,
                                           double stopband_hz, float As) {
    const float fc = static_cast<float>(0.5 * (passband_hz + stopband_hz) / fs);
    const float df = static_cast<float>((stopband_hz - passband_hz) / fs);
    unsigned int n = estimate_req_filter_len(df, As);
    if (n < 3)
        n = 3;
    if ((n & 1u) == 0u)
        n += 1; // odd -> linear-phase type I, integer group delay
    std::vector<float> h(n);
    liquid_firdes_kaiser(n, fc, As, 0.0f, h.data());
    double dc = 0.0;
    for (float v : h)
        dc += v;
    if (std::fabs(dc) > 1e-12) {
        const float g = static_cast<float>(1.0 / dc);
        for (float& v : h)
            v *= g;
    }
    return h;
}

// Fixed-frequency second-order notch (RBJ cookbook, transposed direct
// form II). Realised only when the notch sits clear of Nyquist -- otherwise
// it stays inert and step() returns its input unchanged, so a caller can
// build one unconditionally and let the sample rate decide. Used for the
// 19 kHz stereo pilot on the final audio (dormant at a 32 kHz audio rate,
// where 19 kHz is above Nyquist and the audio stages already bury it).
class BiquadNotch {
public:
    BiquadNotch() = default;
    BiquadNotch(double fs, double f0, double q) {
        if (!(f0 > 0.0) || f0 >= 0.47 * fs || !(q > 0.0))
            return; // inert: m_active stays false
        const double w0 = 2.0 * M_PI * f0 / fs;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        m_b0 = static_cast<float>(1.0 / a0);
        m_b1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
        m_b2 = m_b0;
        m_a1 = m_b1;
        m_a2 = static_cast<float>((1.0 - alpha) / a0);
        m_active = true;
    }
    bool active() const { return m_active; }
    float step(float x) {
        if (!m_active)
            return x;
        const float y = m_b0 * x + m_s1;
        m_s1 = m_b1 * x - m_a1 * y + m_s2;
        m_s2 = m_b2 * x - m_a2 * y;
        return y;
    }

private:
    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    float m_s1 = 0.0f, m_s2 = 0.0f;
    bool m_active = false;
};

// 19 kHz stereo-pilot phase-locked loop. Wraps
// one nco_crcf locked to the analytic pilot coming out of the composite
// band-pass, and regenerates the 38 kHz L-R subcarrier by phase doubling.
// Replaces the old normalise-and-square carrier regen: a squarer doubles
// the pilot's phase-noise variance and gives no coherent gain, whereas a
// narrow PLL rejects noise at the C/N where `auto` still wants stereo and
// yields a real lock flag (phase-error coherence) instead of a bare power
// test. The carrier polarity is a compiled constant folded from the pilot
// BPF group delay + discriminator sign -- no more FMRX_STEREO_INVERT knob.
class PilotPll {
public:
    PilotPll() = default;
    // `carrier_phase` rad: fixed rotation applied to the doubled phase so
    // the regenerated sin(2 w19 t) lines up with the transmitted L-R DSB.
    PilotPll(double mpx_rate, double pilot_hz, double loop_bw_hz,
             float carrier_phase)
        : m_mpx_rate(static_cast<float>(mpx_rate)),
          m_cos_phi(std::cos(carrier_phase)),
          m_sin_phi(std::sin(carrier_phase)) {
        m_nco = nco_crcf_create(LIQUID_VCO);
        nco_crcf_set_frequency(
            m_nco, static_cast<float>(2.0 * M_PI * pilot_hz / mpx_rate));
        nco_crcf_pll_set_bandwidth(
            m_nco, static_cast<float>(2.0 * M_PI * loop_bw_hz / mpx_rate));
        m_lock_a = alpha_from_ms(40.0, mpx_rate);
    }
    PilotPll(const PilotPll&) = delete;
    PilotPll& operator=(const PilotPll&) = delete;
    ~PilotPll() {
        if (m_nco)
            nco_crcf_destroy(m_nco);
    }

    // Advance one sample given the analytic pilot band-pass output `p`.
    // Returns the regenerated 38 kHz subcarrier (unit amplitude).
    float step(cfloat p) {
        cfloat ref;
        nco_crcf_cexpf(m_nco, &ref);
        const float dot = p.real() * ref.real() + p.imag() * ref.imag();
        const float cross = p.imag() * ref.real() - p.real() * ref.imag();
        const float perr = std::atan2(cross, dot);
        const float mag =
            std::sqrt(p.real() * p.real() + p.imag() * p.imag());
        // cos(perr) without a trig call: dot / (|p| |ref|), and |ref| == 1.
        const float coh_inst = (mag > 1e-12f) ? dot / mag : 0.0f;
        m_coh += m_lock_a * (coh_inst - m_coh);
        // 38 kHz = sin(2 theta + phi); double-angle straight off `ref`.
        const float c2 = ref.real() * ref.real() - ref.imag() * ref.imag();
        const float s2 = 2.0f * ref.real() * ref.imag();
        m_carrier = s2 * m_cos_phi + c2 * m_sin_phi;
        nco_crcf_pll_step(m_nco, perr);
        nco_crcf_step(m_nco);
        return m_carrier;
    }
    float carrier38() const { return m_carrier; }
    // Old normalise-and-square carrier regen: unit-normalise the BPF'd
    // pilot phasor and double its phase (-Im(pn^2) = sin(2 w19 t)). No loop
    // state, no coherence estimate. The `stereo.pilot_pll: false` fallback.
    float carrier_square(cfloat p) const {
        const float mag =
            std::sqrt(p.real() * p.real() + p.imag() * p.imag()) + 1e-12f;
        const cfloat pn = p / mag;
        return -2.0f * pn.real() * pn.imag();
    }
    // Phase-error coherence, EMA-smoothed: ~1 for a phase-locked carrier,
    // ~0 for noise in the pilot band. The lock detector thresholds this.
    float coherence() const { return m_coh; }
    // Tracked pilot frequency (Hz) -- 19 kHz when locked; the offset from
    // nominal is an independent cross-check on the AFC.
    float freq_hz() const {
        return m_nco ? nco_crcf_get_frequency(m_nco) * m_mpx_rate /
                           (2.0f * static_cast<float>(M_PI))
                     : 0.0f;
    }

private:
    nco_crcf m_nco = nullptr;
    float m_mpx_rate = 0.0f;
    float m_cos_phi = 1.0f, m_sin_phi = 0.0f;
    float m_lock_a = 0.0f, m_coh = 0.0f, m_carrier = 0.0f;
};

// Impulsive-click suppressor for the FM discriminator output (the
// always-safe half). Near the FM threshold
// the phase-increment detector occasionally wraps a full turn in one
// sample -- an impulsive spike several times the legit peak deviation.
// Track a slow local mean `mu` and a MAD-like robust scale `s` of the
// residual (updated only on clean samples); when |d - mu| exceeds
// sigma*s (and sigma*s clears a floor, so a dead-quiet passage can't
// declare every sample a click), hold the last passed value. Bounded run
// length so a genuine hot transient is never held more than a few us.
class Declicker {
public:
    Declicker() = default;
    Declicker(double fs, float sigma, float floor)
        : m_sigma(sigma), m_floor(floor) {
        m_mu_a = alpha_from_ms(5.0, fs);
        m_scale_a = alpha_from_ms(2.0, fs);
    }
    float step(float d) {
        const float dev = std::fabs(d - m_mu);
        const bool click = m_scale > 0.0f && dev > m_sigma * m_scale &&
                           m_sigma * m_scale > m_floor && m_run < kMaxRun;
        float out;
        if (click) {
            // Linear extrapolation from the last two clean outputs -- a
            // gentler patch than a flat hold (less spectral splatter from
            // the step discontinuity).
            out = 2.0f * m_last - m_prev2;
            ++m_run;
            ++m_clicks;
        } else {
            out = d;
            m_run = 0;
            m_mu += m_mu_a * (d - m_mu);
            m_scale += m_scale_a * (std::fabs(d - m_mu) - m_scale);
        }
        m_prev2 = m_last;
        m_last = out;
        return out;
    }
    unsigned long long clicks() const { return m_clicks; }

private:
    static constexpr int kMaxRun = 3;
    float m_sigma = 4.0f, m_floor = 0.0f;
    float m_mu_a = 0.0f, m_scale_a = 0.0f;
    float m_mu = 0.0f, m_scale = 0.0f, m_last = 0.0f, m_prev2 = 0.0f;
    int m_run = 0;
    unsigned long long m_clicks = 0;
};

// PLL / feedback FM demodulator (the opt-in half -- `fm.demod: pll`,
// mono only, experimental). One nco_crcf tracks
// the instantaneous phase of the pre-demod IF; its loop-filter output
// frequency is the demodulated FM. The 2nd-order loop's noise bandwidth is
// narrower than the open-loop arctan detector's, which is where the
// textbook ~2-3 dB of threshold extension comes from -- provided the loop
// BW sits below the pre-demod noise bandwidth yet above the modulation's
// occupied width. Returns instantaneous frequency in rad/sample, so the
// caller scales it by the same m_k = channel_rate/(2 pi deviation) the
// atan2 path uses and every downstream consumer is unaffected.
class PllFmDemod {
public:
    PllFmDemod() = default;
    PllFmDemod(double fs, double loop_bw_hz) {
        m_nco = nco_crcf_create(LIQUID_VCO);
        nco_crcf_set_frequency(m_nco, 0.0f);
        float bw = static_cast<float>(2.0 * M_PI * loop_bw_hz / fs);
        bw = std::max(1e-4f, std::min(bw, 0.8f)); // liquid stable range
        nco_crcf_pll_set_bandwidth(m_nco, bw);
    }
    PllFmDemod(const PllFmDemod&) = delete;
    PllFmDemod& operator=(const PllFmDemod&) = delete;
    ~PllFmDemod() {
        if (m_nco)
            nco_crcf_destroy(m_nco);
    }
    float step(cfloat x) {
        cfloat ref;
        nco_crcf_cexpf(m_nco, &ref);
        const float dot = x.real() * ref.real() + x.imag() * ref.imag();
        const float cross = x.imag() * ref.real() - x.real() * ref.imag();
        const float perr = std::atan2(cross, dot);
        nco_crcf_pll_step(m_nco, perr);
        const float f = nco_crcf_get_frequency(m_nco);
        nco_crcf_step(m_nco);
        return f;
    }

private:
    nco_crcf m_nco = nullptr;
};

// Short look-ahead brickwall peak limiter -- the final stage before int16
// quantisation. Feed-forward: an instantaneous-attack / exponential-release
// peak envelope of the *input* drives a gain that ramps in with a time
// constant ~1/5 of the look-ahead, so the reduction has converged by the
// time the offending sample leaves the delay line. Output is then hard-
// clamped to +/-ceiling as a sub-sample true-peak safety (essentially never
// engaged once the ramp has settled). Reaches `ceiling` cleanly without the
// per-transient harmonic distortion of a memoryless tanh knee. step1() is
// mono; step2() limits an L/R pair on one shared gain (no image shift).
class LookaheadLimiter {
public:
    LookaheadLimiter() = default;
    LookaheadLimiter(double fs, double lookahead_ms, double release_ms,
                     float ceiling, int channels)
        : m_ceiling(ceiling) {
        m_delay = std::max<size_t>(
            1, static_cast<size_t>(std::lround(lookahead_ms * 1e-3 * fs)));
        const int nch = channels < 2 ? 1 : 2;
        m_buf.assign(m_delay * static_cast<size_t>(nch), 0.0f);
        m_attack_a = alpha_from_ms(lookahead_ms * 0.2, fs);
        m_release_a = alpha_from_ms(release_ms, fs);
    }

    float step1(float x) {
        advance_gain(std::fabs(x));
        const float y = m_buf[m_pos];
        m_buf[m_pos] = x;
        if (++m_pos == m_delay)
            m_pos = 0;
        return clamp_ceiling(y * m_gain);
    }

    void step2(float l, float r, float& outL, float& outR) {
        advance_gain(std::max(std::fabs(l), std::fabs(r)));
        const size_t i = m_pos * 2;
        outL = clamp_ceiling(m_buf[i] * m_gain);
        outR = clamp_ceiling(m_buf[i + 1] * m_gain);
        m_buf[i] = l;
        m_buf[i + 1] = r;
        if (++m_pos == m_delay)
            m_pos = 0;
    }

private:
    void advance_gain(float peak) {
        m_env = (peak > m_env) ? peak
                               : m_env + m_release_a * (peak - m_env);
        const float want = (m_env > m_ceiling) ? m_ceiling / m_env : 1.0f;
        const float a = (want < m_gain) ? m_attack_a : m_release_a;
        m_gain += a * (want - m_gain);
    }
    float clamp_ceiling(float y) const {
        return std::max(-m_ceiling, std::min(m_ceiling, y));
    }

    std::vector<float> m_buf;
    size_t m_delay = 1, m_pos = 0;
    float m_ceiling = 1.0f;
    float m_attack_a = 1.0f, m_release_a = 1.0f;
    float m_env = 0.0f, m_gain = 1.0f;
};

// Real FIR low-pass followed by integer decimation, carrying filter
// history and decimation phase across process() calls.
class RealLpfDecimator {
public:
    RealLpfDecimator() = default;

    RealLpfDecimator(double fs, double passband_hz, double stopband_hz, float As,
                     int decim)
        : m_decim(decim) {
        std::vector<float> h = kaiser_lpf_unity(fs, passband_hz, stopband_hz, As);
        m_filt = firfilt_rrrf_create(h.data(), static_cast<unsigned int>(h.size()));
    }

    RealLpfDecimator(const RealLpfDecimator&) = delete;
    RealLpfDecimator& operator=(const RealLpfDecimator&) = delete;
    RealLpfDecimator(RealLpfDecimator&& o) noexcept { *this = std::move(o); }
    RealLpfDecimator& operator=(RealLpfDecimator&& o) noexcept {
        if (this != &o) {
            destroy();
            m_filt = o.m_filt;
            m_decim = o.m_decim;
            m_phase = o.m_phase;
            o.m_filt = nullptr;
        }
        return *this;
    }
    ~RealLpfDecimator() { destroy(); }

    // Appends decimated output to `out`.
    void process(const float* x, size_t n, std::vector<float>& out) {
        for (size_t i = 0; i < n; ++i) {
            firfilt_rrrf_push(m_filt, x[i]);
            if (m_phase == 0) {
                float y;
                firfilt_rrrf_execute(m_filt, &y);
                out.push_back(y);
                m_phase = m_decim - 1;
            } else {
                --m_phase;
            }
        }
    }

private:
    void destroy() {
        if (m_filt)
            firfilt_rrrf_destroy(m_filt);
        m_filt = nullptr;
    }

    firfilt_rrrf m_filt = nullptr;
    int m_decim = 1;
    int m_phase = 0; // samples until the next kept output
};

} // namespace fmrx
