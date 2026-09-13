// Synthetic (no-SDR) test/measurement modes: --selftest, --stereo-selftest,
// --sweep, --snr-cal, --demod-cal, --scan. All push a synthesised wideband complex
// signal through the *real* channelizer + MonoStation chain (or, for
// --scan, the real periodogram/carrier-detector) and check the result
// against an analytic expectation -- see main.cpp's usage banner for what
// each one measures.
//
// Kept out of main.cpp to shrink that file; nothing here is called from
// the live (`run_live`) path.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "channelizer.hpp"
#include "mono_station.hpp"
#include "plan.hpp"
#include "scan.hpp"
#include "spectrum.hpp"

namespace fmrx {

// First mono station -- the analysis in --selftest / --sweep assumes a
// single-channel capture.
inline int first_mono_station(const fmrx::Plan& plan) {
    for (size_t i = 0; i < plan.stations.size(); ++i)
        if (!plan.stations[i].stereo)
            return static_cast<int>(i);
    return -1;
}

// First stereo (stereo: true / auto) station -- for --stereo-selftest.
inline int first_stereo_station(const fmrx::Plan& plan) {
    for (size_t i = 0; i < plan.stations.size(); ++i)
        if (plan.stations[i].stereo)
            return static_cast<int>(i);
    return -1;
}

// --- synthetic wideband generator ------------------------------------
// One FM station at (station.freq - rx_lo) offset, modulated by a sum of
// audio tones, at proc_rate, wideband complex. Everything else is empty
// spectrum.
struct Tone {
    double hz;
    double amp; // fraction of full deviation
};

// `cn_db`: if finite, add complex white Gaussian noise scaled so the
// carrier-to-noise ratio *in one channelizer bin's bandwidth*
// (st.channel_rate_hz) is `cn_db`. The noise is spread white across this
// generator's full rate `fs`, so post-channelization the in-bin C/N lands
// on target regardless of whether the caller runs at proc_rate or (the
// FMRX_NOCHAN path) straight at channel_rate.
inline std::vector<cfloat> synth_wideband(
    const fmrx::Plan& plan, const fmrx::StationPlan& st, size_t nsamp,
    const std::vector<Tone>& tones,
    double cn_db = std::numeric_limits<double>::infinity()) {
    const double fs = static_cast<double>(plan.proc_rate_hz);
    const double f_offset = static_cast<double>(st.freq_hz - plan.sdr_rx_lo_hz);
    const double dev = st.fm_deviation_hz;

    std::vector<double> phase(tones.size(), 0.0), w(tones.size());
    for (size_t i = 0; i < tones.size(); ++i)
        w[i] = 2.0 * M_PI * tones[i].hz / fs;

    const bool add_noise = std::isfinite(cn_db);
    double nsigma = 0.0; // per real component
    if (add_noise) {
        const double carrier_pow = 0.25; // |0.5 * e^{j phi}|^2
        const double cnr = std::pow(10.0, cn_db / 10.0);
        const double band_ratio = fs / static_cast<double>(st.channel_rate_hz);
        nsigma = std::sqrt(carrier_pow / cnr * band_ratio / 2.0);
    }
    std::mt19937 rng(0xC0FFEEu ^
                     static_cast<unsigned>(std::llround(cn_db * 1000.0)));
    std::normal_distribution<double> gauss(0.0, 1.0);

    std::vector<cfloat> x(nsamp);
    double fm_phase = 0.0;
    const double w_off = 2.0 * M_PI * f_offset / fs;
    for (size_t n = 0; n < nsamp; ++n) {
        double msg = 0.0;
        for (size_t i = 0; i < tones.size(); ++i) {
            msg += tones[i].amp * std::sin(phase[i]);
            phase[i] += w[i];
        }
        fm_phase += 2.0 * M_PI * dev * msg / fs;
        const double ph = fm_phase + w_off * static_cast<double>(n);
        double re = 0.5 * std::cos(ph), im = 0.5 * std::sin(ph);
        if (add_noise) {
            re += nsigma * gauss(rng);
            im += nsigma * gauss(rng);
        }
        x[n] = cfloat(static_cast<float>(re), static_cast<float>(im));
    }
    return x;
}

// Synthetic FM stereo multiplex: L carries a tone at fL, R a tone at fR.
// Builds the baseband composite  (L+R)/2 + 0.1*pilot + (L-R)/2 * sin(2*wp)
// then FM-modulates it onto the station's RF offset. No pre-emphasis (the
// decoder still de-emphasises; that rolls both test tones equally so it
// doesn't affect a channel-separation measurement).
inline std::vector<cfloat> synth_stereo_wideband(const fmrx::Plan& plan,
                                                 const fmrx::StationPlan& st,
                                                 size_t nsamp, double fL, double fR,
                                                 double dev_frac) {
    const double fs = static_cast<double>(plan.proc_rate_hz);
    const double f_offset = static_cast<double>(st.freq_hz - plan.sdr_rx_lo_hz);
    const double dev = st.fm_deviation_hz;
    const double wL = 2.0 * M_PI * fL / fs, wR = 2.0 * M_PI * fR / fs;
    const double wp = 2.0 * M_PI * 19000.0 / fs;
    const double w_off = 2.0 * M_PI * f_offset / fs;

    std::vector<cfloat> x(nsamp);
    double phL = 0.0, phR = 0.0, php = 0.0, fm_phase = 0.0;
    for (size_t n = 0; n < nsamp; ++n) {
        const double l = dev_frac * std::sin(phL);
        const double r = dev_frac * std::sin(phR);
        const double msg = 0.5 * (l + r) + 0.1 * std::sin(php) +
                           0.5 * (l - r) * std::sin(2.0 * php);
        phL += wL;
        phR += wR;
        php += wp;
        fm_phase += 2.0 * M_PI * dev * msg / fs;
        const double ph = fm_phase + w_off * static_cast<double>(n);
        x[n] = cfloat(static_cast<float>(0.5 * std::cos(ph)),
                      static_cast<float>(0.5 * std::sin(ph)));
    }
    return x;
}

// Single-frequency amplitude estimate (Goertzel-style DFT bin) over a
// float window. Returns the peak amplitude of a real sinusoid at `freq`.
// `n` (0 = all) caps the sample count -- callers that need an integer
// number of `freq` cycles pass the trimmed length so the unwindowed sum
// doesn't leak.
inline double tone_amplitude(const std::vector<double>& x, double freq, int rate,
                             size_t n = 0) {
    if (n == 0 || n > x.size())
        n = x.size();
    if (n < 8)
        return 0.0;
    const double w = 2.0 * M_PI * freq / rate;
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < n; ++k) {
        re += x[k] * std::cos(w * k);
        im += x[k] * std::sin(w * k);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

// Signal metadata read off the MonoStation after a synthetic run.
struct SynthResult {
    double snr_cal_db = 0.0;   // calibrated, program-independent SNR
    double snr_proxy_db = 0.0; // raw ultrasonic-noise-ratio proxy
    double snr_knoise = 0.0;   // effective K_noise the station used
};

// Run `seconds` of a synthetic FM signal (station `st`, modulated by
// `tones`) through the real channelizer + MonoStation chain and return
// the recovered audio, transient-trimmed and normalised to +/-1.0.
// `cn_db` (if finite) injects AWGN for a target in-bin C/N; `out` (if
// non-null) is filled with the station's final signal metadata.
inline std::vector<double> process_synthetic(
    const fmrx::Plan& plan, const fmrx::StationPlan& st,
    const std::vector<Tone>& tones, double seconds,
    double cn_db = std::numeric_limits<double>::infinity(),
    SynthResult* out = nullptr) {
    const size_t block = static_cast<size_t>(plan.samples_per_block);
    const size_t total =
        static_cast<size_t>(seconds * plan.proc_rate_hz / block + 1) * block;

    // FMRX_NOCHAN: skip the channelizer entirely -- synthesise the FM
    // signal straight at channel_rate with the carrier already at DC, and
    // feed it into a MonoStation whose fine mixer is a no-op. Isolates the
    // per-station chain (predemod / discriminator / de-emph / audio LPFs)
    // from the channelizer so a frequency-response defect can be pinned to
    // one side.
    if (std::getenv("FMRX_NOCHAN") != nullptr) {
        // Synthesise the FM signal straight at channel_rate with the
        // carrier already at DC, and feed a MonoStation whose fine mixer
        // is a no-op. Skips the channelizer, so a frequency-response
        // defect can be pinned to one side of it.
        fmrx::Plan bb = plan;
        fmrx::StationPlan bst = st;
        bb.proc_rate_hz = st.channel_rate_hz;
        bst.freq_hz = plan.sdr_rx_lo_hz; // zero RF offset -> carrier at DC
        bst.fine_hz = 0.0;
        fmrx::MonoStation bband(bst, plan.udp_host, plan.udp_mtu, true);
        const size_t bsize = block / static_cast<size_t>(plan.decim_p);
        const size_t btotal = total / static_cast<size_t>(plan.decim_p);
        std::vector<cfloat> x = synth_wideband(bb, bst, btotal, tones, cn_db);
        for (size_t off = 0; off + bsize <= btotal; off += bsize)
            bband.process(x.data() + off, bsize);
        if (out) {
            out->snr_cal_db = bband.snr_db();
            out->snr_proxy_db = bband.squelch_metric_db();
            out->snr_knoise = bband.snr_knoise();
        }
        std::vector<double> res;
        const size_t g = bband.captured.size();
        const size_t sk =
            std::min(g, static_cast<size_t>(st.audio_rate_hz / 10));
        for (size_t i = sk; i < g; ++i)
            res.push_back(bband.captured[i] / 32768.0);
        return res;
    }

    std::vector<cfloat> wb = synth_wideband(plan, st, total, tones, cn_db);

    fmrx::Channelizer chan(plan.num_channels, plan.decim_p,
                           plan.proto_semilen_m, plan.atten_db);
    chan.set_active_bins({st.bin});
    fmrx::MonoStation station(st, plan.udp_host, plan.udp_mtu,
                              /*capture_only=*/true);

    for (size_t off = 0; off + block <= total; off += block) {
        chan.channelize(wb.data() + off, block);
        const auto& b = chan.bin(st.bin);
        station.process(b.data(), b.size());
    }

    if (out) {
        out->snr_cal_db = station.snr_db();
        out->snr_proxy_db = station.squelch_metric_db();
        out->snr_knoise = station.snr_knoise();
    }

    const size_t got = station.captured.size();
    const size_t skip =
        std::min(got, static_cast<size_t>(st.audio_rate_hz / 10)); // 100 ms
    std::vector<double> a;
    a.reserve(got - skip);
    for (size_t i = skip; i < got; ++i)
        a.push_back(station.captured[i] / 32768.0);
    return a;
}

// Ideal 50 us (or whatever tau) FM de-emphasis magnitude, in dB, at `f`.
inline double deemph_db(double f, double tau) {
    const double fc = 1.0 / (2.0 * M_PI * tau);
    return -10.0 * std::log10(1.0 + (f / fc) * (f / fc));
}

// "Truth" SNR for the calibration sweep: power of the reference tone vs.
// everything else, measured on the recovered (de-emphasised) audio. With
// only a single tone + AWGN present that residual is the noise floor.
inline double output_audio_snr(const std::vector<double>& x, double tone_hz, int rate) {
    if (x.size() < 64)
        return 0.0;
    // Trim to a whole number of tone cycles. tone_amplitude() is an
    // unwindowed DFT bin and `total` below is a plain variance, so a
    // fractional last cycle leaks the (large) reference tone into the
    // residual and floors the measured SNR -- ~44 dB for a ~0.6-cycle
    // remainder. Whether the capture happens to land on an integer cycle
    // count is pure luck: the length is proc_rate/block-dependent, so whether
    // it lands exactly depends on the sample rate in play.
    size_t n = x.size();
    const double period = static_cast<double>(rate) / tone_hz; // samples/cycle
    if (period > 1.0) {
        const size_t keep = static_cast<size_t>(
            std::llround(std::floor(n / period) * period));
        if (keep >= 64)
            n = keep;
    }
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i)
        mean += x[i];
    mean /= static_cast<double>(n);
    double total = 0.0;
    for (size_t i = 0; i < n; ++i)
        total += (x[i] - mean) * (x[i] - mean);
    total /= static_cast<double>(n);
    const double amp = tone_amplitude(x, tone_hz, rate, n);
    const double tone_pow = 0.5 * amp * amp;
    const double resid = std::max(total - tone_pow, 1e-15);
    return 10.0 * std::log10(tone_pow / resid);
}

// Sweep single audio tones through the synthetic chain and print the
// recovered amplitude vs frequency against the theoretical de-emphasis
// curve. This is the end-to-end audio frequency response: it catches a
// mis-tuned / doubled de-emphasis or an audio low-pass that droops into
// the passband -- the "sounds bassy" class of bug.
inline int run_sweep(const fmrx::Plan& plan, double seconds) {
    const int mi = first_mono_station(plan);
    if (mi < 0) {
        std::fprintf(stderr, "sweep: no mono stations in plan\n");
        return 1;
    }
    const fmrx::StationPlan& st = plan.stations[mi];
    const double tau = st.fm_tau_s;
    std::printf("audio frequency response  (station %s, tau %.0f us, "
                "reference 1 kHz)\n",
                st.label.c_str(), tau * 1e6);
    std::printf("   freq Hz   measured dB   de-emph dB    delta dB\n");

    const std::vector<double> freqs = {50,   75,   100,  150,   200,  250,
                                       300,  500,  1000, 2000,  3000, 4000,
                                       6000, 8000, 10000, 12000, 14000, 15000};
    // small deviation so the FM sidebands stay well inside every RF-side
    // filter -- isolates the audio path (discriminator + de-emph + LPFs).
    const double dev_frac = 0.05;

    // Anchor at 1 kHz (flat, well inside every passband). Measured first so
    // it's available for the frequencies swept below it.
    const double ref_db = 20.0 * std::log10(std::max(
        tone_amplitude(process_synthetic(plan, st, {{1000.0, dev_frac}}, seconds),
                       1000.0, st.audio_rate_hz),
        1e-9));

    double worst_delta = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const double f = freqs[i];
        std::vector<double> a =
            process_synthetic(plan, st, {{f, dev_frac}}, seconds);
        const double amp = tone_amplitude(a, f, st.audio_rate_hz);
        const double db = 20.0 * std::log10(std::max(amp, 1e-9));
        if (std::getenv("FMRX_DEBUG")) {
            double mean = 0.0, sq = 0.0;
            for (double v : a) mean += v;
            mean /= a.size();
            for (double v : a) sq += (v - mean) * (v - mean);
            std::fprintf(stderr,
                         "    f=%.0f  N=%zu  mean=%.5f rms=%.5f  |amp@f|=%.5f "
                         "|amp@1k|=%.5f\n",
                         f, a.size(), mean, std::sqrt(sq / a.size()), amp,
                         tone_amplitude(a, 1000.0, st.audio_rate_hz));
        }
        const double rel = db - ref_db;
        const double want = deemph_db(f, tau) - deemph_db(1000.0, tau);
        const double delta = rel - want;
        // 14-15 kHz sits in the audio LPF's transition band; allow it a
        // little more slack than the rest of the band.
        const double tol = (f >= 13500.0) ? 1.5 : 1.0;
        std::printf("  %7.0f   %+9.2f   %+9.2f   %+8.2f%s\n", f, rel, want,
                    delta, std::fabs(delta) > tol ? "   <-- off" : "");
        if (std::fabs(delta) > std::fabs(worst_delta))
            worst_delta = delta;
    }

    // The whole 50 Hz - 15 kHz band should track the ideal de-emphasis
    // curve. A large deviation means a mis-scaled/doubled de-emphasis or an
    // audio low-pass whose band edges droop into the passband.
    const bool ok = std::fabs(worst_delta) <= 1.5;
    std::printf("  worst deviation from ideal de-emphasis: %+.2f dB  -> %s\n",
                worst_delta, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Sweep carrier-to-noise ratio through the synthetic chain and tabulate
// the calibrated `snr_db` against the true recovered-audio SNR. Above the
// FM threshold knee (~6-9 dB C/N) the calibrated metric should track truth
// within a couple of dB. `K_fit = 10^((referenced - true)/10)` is the
// factor to multiply the compiled kSnrKNoiseDefault by; its mean over the
// 9-33 dB C/N rows re-centres the metric (== 1.0 when already fitted).
inline int run_snr_cal(const fmrx::Plan& plan, double seconds) {
    if (plan.stations.empty()) {
        std::fprintf(stderr, "snr-cal: no stations in plan\n");
        return 1;
    }
    // K_noise is a property of the DSP chain, not the station: deviation,
    // tau, audio band, channel_rate and the pre-demod transition are all
    // global. So this doesn't run the live station list -- it builds one
    // synthetic mono chain from the best-centred station's plan (smallest
    // |fine_hz|, least channelizer-edge roll-off) and forces it mono so an
    // all-stereo list still works. The sweep is offline and deterministic
    // (fixed-seed AWGN, no real-time loop) -- host CPU load changes only
    // how long it takes.
    size_t best = 0;
    for (size_t i = 1; i < plan.stations.size(); ++i)
        if (std::fabs(plan.stations[i].fine_hz) <
            std::fabs(plan.stations[best].fine_hz))
            best = i;
    fmrx::StationPlan st = plan.stations[best];
    st.stereo_mode = fmrx::StereoMode::Mono;
    st.stereo = false; // snr_knoise (global) is kept as resolved

    std::printf("SNR calibration sweep  (chain from station %s, tau %.0f us)\n",
                st.label.c_str(), st.fm_tau_s * 1e6);
    std::printf("  reference: 1 kHz tone at full +/-%d Hz deviation, "
                "C/N in a %d Hz channel bin\n",
                st.fm_deviation_hz, st.channel_rate_hz);
    std::printf("  'referenced dB' is the live calibrated snr_db (current "
                "K_noise applied);\n"
                "  bake  kSnrKNoiseDefault *= mean(K_fit)  to re-centre it.\n");
    std::printf("   C/N dB   proxy dB   referenced dB   true SNR dB   delta   "
                "K_fit\n");

    const std::vector<double> cn_list = {0,  3,  6,  9,  12, 15, 18,
                                        21, 24, 27, 30, 35, 40};
    double kfit_sum = 0.0, worst = 0.0, k_cur = 0.0;
    int kfit_n = 0;
    bool assert_fail = false;
    for (double cn : cn_list) {
        SynthResult m;
        std::vector<double> a =
            process_synthetic(plan, st, {{1000.0, 1.0}}, seconds, cn, &m);
        const double truth = output_audio_snr(a, 1000.0, st.audio_rate_hz);
        k_cur = m.snr_knoise;
        const double kfit = std::pow(10.0, (m.snr_cal_db - truth) / 10.0);
        const double delta = m.snr_cal_db - truth;
        // Fit above the FM threshold knee (~6-9 dB C/N here) and below the
        // point where int16 capture quantization floors the guard band.
        const bool in_fit = cn >= 9.0 && cn <= 33.0;
        std::printf("  %6.1f   %8.1f   %11.1f   %10.1f   %+6.1f   %7.4g%s\n", cn,
                    m.snr_proxy_db, m.snr_cal_db, truth, delta, kfit,
                    in_fit ? "" : "   (outside fit band)");
        if (in_fit) {
            kfit_sum += kfit;
            ++kfit_n;
            if (std::fabs(delta) > std::fabs(worst))
                worst = delta;
            if (std::fabs(delta) > 2.0)
                assert_fail = true;
        }
    }
    if (kfit_n > 0) {
        const double kmean = kfit_sum / kfit_n;
        std::printf("\n  K_noise in use: %.4g   mean K_fit (9-33 dB C/N): %.4g\n",
                    k_cur, kmean);
        std::printf("  -> to re-centre, set  snr.k_noise: %.4g  in config.yml "
                    "(or re-bake kSnrKNoiseDefault)\n",
                    k_cur * kmean);
        std::printf("  worst |referenced - true| in the fit band: %+.1f dB "
                    "-> %s\n",
                    worst, assert_fail ? "FAIL (re-centre K_noise)" : "PASS");
    }
    return assert_fail ? 1 : 0;
}

// Compare the FM demod options over the threshold knee: run the
// synthetic mono chain for `discriminator`,
// `discriminator + declick` and `pll`, and tabulate the true
// recovered-audio SNR (the same truth --snr-cal fits K_noise against) at
// each C/N. The declick path is asserted (it must not regress above the
// knee); the pll path is printed for a human to judge per baseline.
inline int run_demod_cal(const fmrx::Plan& plan, double seconds) {
    if (plan.stations.empty()) {
        std::fprintf(stderr, "demod-cal: no stations in plan\n");
        return 1;
    }
    // Same chain-selection rule as run_snr_cal: the best-centred station,
    // forced mono so an all-stereo list works.
    size_t best = 0;
    for (size_t i = 1; i < plan.stations.size(); ++i)
        if (std::fabs(plan.stations[i].fine_hz) <
            std::fabs(plan.stations[best].fine_hz))
            best = i;
    fmrx::StationPlan base = plan.stations[best];
    base.stereo_mode = fmrx::StereoMode::Mono;
    base.stereo = false;

    if (std::getenv("FMRX_FM_DEMOD") || std::getenv("FMRX_FM_DECLICK"))
        std::printf("  note: FMRX_FM_DEMOD / FMRX_FM_DECLICK are set and "
                    "override the per-mode plan -- unset them for a clean "
                    "comparison\n");

    struct Mode {
        const char* name;
        const char* demod;
        bool declick;
    };
    const Mode modes[3] = {
        {"discriminator", "discriminator", false},
        {"+declick", "discriminator", true},
        {"pll", "pll", false},
    };
    const std::vector<double> cn_list = {0.0, 3.0, 6.0, 9.0, 12.0};
    const size_t NC = cn_list.size();

    std::printf("FM demod threshold comparison  (station %s, tau %.0f us)\n",
                base.label.c_str(), base.fm_tau_s * 1e6);
    std::printf("  true recovered-audio SNR (dB) of a 1 kHz reference tone "
                "vs in-bin C/N\n");
    std::printf("   C/N dB   discriminator   +declick        pll        "
                "declick d    pll d\n");

    std::vector<std::vector<double>> truth(
        3, std::vector<double>(NC, 0.0));
    for (int mi = 0; mi < 3; ++mi) {
        fmrx::StationPlan st = base;
        st.fm_demod = modes[mi].demod;
        st.fm_declick = modes[mi].declick;
        for (size_t ci = 0; ci < NC; ++ci) {
            std::vector<double> a = process_synthetic(
                plan, st, {{1000.0, 1.0}}, seconds, cn_list[ci]);
            truth[mi][ci] =
                output_audio_snr(a, 1000.0, st.audio_rate_hz);
        }
    }

    bool regress = false, helps = false;
    for (size_t ci = 0; ci < NC; ++ci) {
        const double dd = truth[1][ci] - truth[0][ci];
        const double dp = truth[2][ci] - truth[0][ci];
        std::printf("  %6.1f   %11.1f   %11.1f   %11.1f   %+8.1f   %+8.1f\n",
                    cn_list[ci], truth[0][ci], truth[1][ci], truth[2][ci], dd,
                    dp);
        // Regression: declick losing ground anywhere from 6 dB C/N up
        // (above the dense-click region -- a fire there is a false positive).
        if (cn_list[ci] >= 6.0 && dd < -0.7)
            regress = true;
        if (cn_list[ci] >= 3.0 && cn_list[ci] <= 9.0 && dd >= 0.2)
            helps = true;
    }
    // Hard assertions: declick must not lose ground above the knee, and
    // must show *some* gain in it. The synthetic AWGN chain understates
    // real off-air click density, so these are deliberately loose.
    const bool ok = !regress && helps;
    std::printf("\n  declick: %s from 6 dB C/N up; %s in the 3-9 dB knee\n",
                regress ? "REGRESSION (>0.7 dB)" : "no regression",
                helps ? "measurable gain (>=0.2 dB)" : "no measurable gain");
    std::printf("  pll: experimental -- judge the 'pll d' column per "
                "baseline\n");
    std::printf("  -> %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Offline band scan: sum one FM carrier per in-plan station into a
// synthetic wideband buffer, push it through the real periodogram +
// carrier detector (spectrum.hpp / scan.hpp -- the exact path GET
// /api/scan uses, minus the SDR), and print what came back. Exit 0 only
// if every station was recovered, so this doubles as a regression check
// on the spgram frequency mapping.
inline int run_scan(const fmrx::Plan& plan, double seconds, bool json) {
    // The averaged periodogram converges in a fraction of a second and the
    // synthetic wideband buffer is O(seconds * samp_rate) per station, so
    // clamp -- more time here buys nothing.
    seconds = std::max(0.2, std::min(seconds, 1.0));
    const unsigned nfft = static_cast<unsigned>(plan.scan_nfft);
    const size_t block = static_cast<size_t>(plan.samples_per_block);
    const size_t total =
        static_cast<size_t>(seconds * plan.proc_rate_hz / block + 1) * block;

    // A 3-tone message at full deviation -> ~160-180 kHz occupied width,
    // like a real broadcast multiplex, so the detector's minimum-width
    // gate is exercised the same way it is on the air.
    const std::vector<Tone> msg = {
        {800.0, 0.45}, {4200.0, 0.35}, {10500.0, 0.20}};

    std::vector<cfloat> wb(total, cfloat(0.0f, 0.0f));
    for (const auto& st : plan.stations) {
        std::vector<cfloat> one = synth_wideband(plan, st, total, msg);
        for (size_t i = 0; i < total; ++i)
            wb[i] += one[i];
    }

    fmrx::Spectrum spec(nfft, static_cast<double>(plan.proc_rate_hz),
                        plan.sdr_rx_lo_hz);
    for (size_t off = 0; off + block <= total; off += block)
        spec.write(wb.data() + off, block);

    std::vector<float> psd;
    spec.get_psd(psd);
    std::vector<fmrx::ScanCarrier> carriers =
        fmrx::detect_carriers(plan, nfft, psd);

    if (json) {
        std::puts(fmrx::scan_json(plan, nfft, psd, carriers).c_str());
        return 0;
    }

    const double bin_khz = plan.proc_rate_hz / static_cast<double>(nfft) / 1e3;
    std::printf(
        "band scan  (synthetic: one FM carrier per station, %.2f s)\n", seconds);
    std::printf("  nfft %u, bin %.1f kHz, span %.3f-%.3f MHz\n", nfft, bin_khz,
                (plan.sdr_rx_lo_hz - plan.proc_rate_hz / 2) / 1e6,
                (plan.sdr_rx_lo_hz + plan.proc_rate_hz / 2) / 1e6);
    std::printf("   carrier MHz   level dBFS   configured\n");
    for (const auto& c : carriers)
        std::printf("  %10.3f   %10.1f   %s\n", c.freq_hz / 1e6, c.power_dbfs,
                    c.configured ? "yes" : "NO");

    int missing = 0;
    for (const auto& st : plan.stations) {
        bool found = false;
        for (const auto& c : carriers)
            if (std::llabs(c.freq_hz - st.freq_hz) <= 100000) {
                found = true;
                break;
            }
        if (!found) {
            std::printf("  MISSING: %s at %.3f MHz not detected\n",
                        st.label.c_str(), st.freq_hz / 1e6);
            ++missing;
        }
    }
    std::printf("  RESULT: %s  (%zu carrier(s), %d missing)\n",
                missing ? "FAIL" : "PASS", carriers.size(), missing);
    return missing ? 1 : 0;
}

inline int run_selftest(const fmrx::Plan& plan, double seconds) {
    const int mi = first_mono_station(plan);
    if (mi < 0) {
        std::fprintf(stderr, "selftest: no mono stations in plan\n");
        return 1;
    }
    const fmrx::StationPlan& st = plan.stations[mi];
    std::printf("selftest station: %s  @ %.3f MHz  (bin %d, fine %.1f Hz)\n",
                st.label.c_str(), st.freq_hz / 1e6, st.bin, st.fine_hz);

    const double tone_a = 1000.0, tone_b = 4000.0;
    std::vector<double> a =
        process_synthetic(plan, st, {{tone_a, 0.6}, {tone_b, 0.4}}, seconds);
    const size_t got = a.size();
    const size_t expect_per_block =
        static_cast<size_t>(plan.samples_per_block) /
        static_cast<size_t>(plan.decim_p) / static_cast<size_t>(st.fm_decim) /
        static_cast<size_t>(st.audio_decim);

    double sig_pow = 0.0;
    for (double v : a)
        sig_pow += v * v;
    sig_pow = a.empty() ? 0.0 : sig_pow / a.size();

    const double amp_a = tone_amplitude(a, tone_a, st.audio_rate_hz);
    const double amp_b = tone_amplitude(a, tone_b, st.audio_rate_hz);
    // Everything that isn't the two injected tones, as a power ratio.
    const double tone_pow = 0.5 * (amp_a * amp_a + amp_b * amp_b);
    const double resid = std::max(sig_pow - tone_pow, 1e-12);
    const double sinad_db = 10.0 * std::log10(tone_pow / resid);

    std::printf("  audio samples out : %zu  (~%zu/block expected)\n", got,
                expect_per_block);
    std::printf("  output RMS        : %.0f  (int16 full-scale 32767)\n",
                std::sqrt(sig_pow) * 32768.0);
    std::printf("  tone %.0f Hz       : %.3f  (injected 0.60, pre de-emph)\n",
                tone_a, amp_a);
    std::printf("  tone %.0f Hz       : %.3f  (injected 0.40, pre de-emph)\n",
                tone_b, amp_b);
    std::printf("  SINAD (2-tone)    : %.1f dB\n", sinad_db);

    // Calibrated SNR: a 1 kHz reference tone at 30 dB C/N should read within
    // a couple of dB of the true recovered-audio SNR.
    SynthResult m;
    std::vector<double> nz =
        process_synthetic(plan, st, {{1000.0, 1.0}}, seconds, 30.0, &m);
    const double true_snr = output_audio_snr(nz, 1000.0, st.audio_rate_hz);
    const double snr_err = std::fabs(m.snr_cal_db - true_snr);
    std::printf("  calibrated SNR    : %.1f dB  (true audio-band %.1f dB @ "
                "30 dB C/N, |err| %.1f)\n",
                m.snr_cal_db, true_snr, snr_err);

    // Loose here (short 2 s capture -> a jittery guard-band EMA); --snr-cal
    // runs the tight +/-2 dB check across the whole C/N sweep.
    const bool ok = got > expect_per_block * 10 && amp_a > 0.30 &&
                    amp_b > 0.10 && sinad_db > 25.0 && snr_err < 3.0;
    std::printf("  RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Push a synthetic FM stereo multiplex (L tone / R tone) through the real
// channelizer + MonoStation stereo path and measure L<->R separation. A
// clean synthetic (no noise) should hit the pilot lock and separate the
// two tones well; a swapped/opposite-phase carrier shows up as negative
// separation (each tone landing in the wrong channel).
inline int run_stereo_selftest(const fmrx::Plan& plan, double seconds) {
    const int si = first_stereo_station(plan);
    if (si < 0) {
        std::fprintf(stderr, "stereo-selftest: no stereo (stereo: true / auto) "
                             "station in plan\n");
        return 1;
    }
    const fmrx::StationPlan& st = plan.stations[si];
    const double fL = 1000.0, fR = 3000.0, dev_frac = 0.3;
    std::printf("stereo-selftest station: %s  @ %.3f MHz  (bin %d, fine %.1f Hz)"
                "\n  L tone %.0f Hz, R tone %.0f Hz\n",
                st.label.c_str(), st.freq_hz / 1e6, st.bin, st.fine_hz, fL, fR);

    const size_t block = static_cast<size_t>(plan.samples_per_block);
    const size_t total =
        static_cast<size_t>(seconds * plan.proc_rate_hz / block + 1) * block;
    std::vector<cfloat> wb =
        synth_stereo_wideband(plan, st, total, fL, fR, dev_frac);

    fmrx::Channelizer chan(plan.num_channels, plan.decim_p, plan.proto_semilen_m,
                           plan.atten_db);
    chan.set_active_bins({st.bin});
    fmrx::MonoStation station(st, plan.udp_host, plan.udp_mtu,
                              /*capture_only=*/true);
    for (size_t off = 0; off + block <= total; off += block) {
        chan.channelize(wb.data() + off, block);
        const auto& b = chan.bin(st.bin);
        station.process(b.data(), b.size());
    }

    // captured is interleaved L,R int16. Drop the first 200 ms (filter +
    // blend-slew transient) and split.
    const size_t frames = station.captured.size() / 2;
    const size_t skip = std::min(frames, static_cast<size_t>(st.audio_rate_hz / 5));
    std::vector<double> L, R;
    for (size_t i = skip; i < frames; ++i) {
        L.push_back(station.captured[2 * i] / 32768.0);
        R.push_back(station.captured[2 * i + 1] / 32768.0);
    }
    if (L.size() < 64) {
        std::printf("  RESULT: FAIL (only %zu frames captured)\n", L.size());
        return 1;
    }

    const double ll = tone_amplitude(L, fL, st.audio_rate_hz);
    const double lr = tone_amplitude(L, fR, st.audio_rate_hz);
    const double rr = tone_amplitude(R, fR, st.audio_rate_hz);
    const double rl = tone_amplitude(R, fL, st.audio_rate_hz);
    const double sep_l = 20.0 * std::log10(std::max(ll, 1e-9) / std::max(lr, 1e-12));
    const double sep_r = 20.0 * std::log10(std::max(rr, 1e-9) / std::max(rl, 1e-12));

    std::printf("  frames out      : %zu  (~%d/s)\n", frames, st.audio_rate_hz);
    std::printf("  pilot_db        : %+.1f  (lock threshold %+.1f)\n",
                station.pilot_db(), st.stereo_pilot_threshold_db);
    std::printf("  stereo_frac     : %.2f  (1.0 = full stereo)\n",
                station.stereo_frac());
    std::printf("  L: tone@%.0f %.3f  tone@%.0f %.3f   -> separation %+.1f dB\n",
                fL, ll, fR, lr, sep_l);
    std::printf("  R: tone@%.0f %.3f  tone@%.0f %.3f   -> separation %+.1f dB\n",
                fR, rr, fL, rl, sep_r);

    // The pilot PLL locks a coherent 38 kHz carrier, so a clean synthetic
    // multiplex clears 25 dB comfortably.
    const bool ok = station.stereo_frac() > 0.8 && station.pilot_lock() &&
                    sep_l > 25.0 && sep_r > 25.0;
    std::printf("  RESULT: %s\n", ok ? "PASS" : "FAIL");
    if (!ok && (sep_l < -6.0 || sep_r < -6.0))
        std::printf("  (tones landed in the wrong channel -- the "
                    "kStereoCarrierPhase constant in stereo_decoder.hpp is "
                    "wrong for this filter/discriminator build)\n");
    return ok ? 0 : 1;
}

} // namespace fmrx
