// Band-scan carrier detection + JSON response. Turns an averaged wideband
// PSD (from spectrum.hpp) into a
// list of likely FM carriers and the wire payload for `GET /api/scan`.
//
// Kept free of the HTTP and DSP objects so it can be unit-tested directly
// (feedmyfm-rx --scan --json runs exactly this on a synthetic multiplex).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "http_server.hpp"
#include "plan.hpp"

namespace fmrx {

struct ScanCarrier {
    long long freq_hz;  // power-weighted centre, snapped to the 100 kHz raster
    float power_dbfs;   // peak PSD across the detected run
    bool configured;    // a stations.yml station (enabled or not) sits within +/-90 kHz
    std::string label;  // that station's label, or "" if not configured
};

namespace scan_detail {

// RF centre frequency of fft-shifted PSD sample i over `span_hz` centred on
// `center_hz` (matches Spectrum::bin_freq_hz).
inline double psd_freq_hz(unsigned i, unsigned nfft, long long center_hz,
                          double span_hz) {
    return static_cast<double>(center_hz) +
           (static_cast<double>(i) / nfft - 0.5) * span_hz;
}

inline std::string fmt2(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return std::string(b);
}

} // namespace scan_detail

// Find the FM carriers in an averaged PSD. `psd_db` is fft-shifted, length
// nfft (see spectrum.hpp).
//
// A contiguous "everything above the floor" run merges neighbouring
// stations into one blob whose centroid is a phantom frequency, so this
// peak-picks instead: smooth to ~70 kHz (one bump per multiplex), then
// keep every smoothed local maximum that clears `scan.threshold_db` over
// the noise floor AND is the strongest sample within +/-90 kHz
// (non-maximum suppression). Each surviving peak is refined to the
// power-weighted centroid of its +/-40 kHz neighbourhood and snapped to
// the 100 kHz raster.
//
// Limitation: two carriers closer than ~180 kHz fall inside one
// suppression window, so only the stronger is reported -- a power scan
// can't reliably split them anyway.
inline std::vector<ScanCarrier> detect_carriers(const Plan& plan, unsigned nfft,
                                                const std::vector<float>& psd_db) {
    std::vector<ScanCarrier> out;
    if (nfft < 16 || psd_db.size() != nfft)
        return out;

    const double span_hz = static_cast<double>(plan.proc_rate_hz);
    const double bin_hz = span_hz / nfft;
    const int N = static_cast<int>(nfft);

    // linear power; notch the DC bin -- SDR LO leakage there would otherwise
    // fake a carrier at the receiver centre.
    std::vector<double> lin(nfft);
    for (unsigned i = 0; i < nfft; ++i)
        lin[i] = std::pow(10.0, psd_db[i] / 10.0);
    lin[nfft / 2] = 0.5 * (lin[nfft / 2 - 1] + lin[nfft / 2 + 1]);

    // ~70 kHz box smoother (runs once per scan; O(nfft * w) is fine).
    const int w = std::max(1, static_cast<int>(std::lround(35000.0 / bin_hz)));
    std::vector<double> sm(nfft);
    for (int c = 0; c < N; ++c) {
        const int lo = std::max(0, c - w);
        const int hi = std::min(N - 1, c + w);
        double a = 0.0;
        for (int k = lo; k <= hi; ++k)
            a += lin[k];
        sm[c] = a / (hi - lo + 1);
    }

    // noise floor: 25th percentile of the smoothed spectrum (robust when a
    // lot of the band is occupied).
    std::vector<double> sorted(sm);
    std::sort(sorted.begin(), sorted.end());
    const double floor_lin = std::max(sorted[nfft / 4], 1e-30);
    const double thr_lin =
        floor_lin * std::pow(10.0, plan.scan_threshold_db / 10.0);

    const int nms = std::max(1, static_cast<int>(std::lround(90000.0 / bin_hz)));
    const int cw = std::max(1, static_cast<int>(std::lround(40000.0 / bin_hz)));
    long long last_snapped = -1;
    for (int i = 1; i < N - 1; ++i) {
        if (sm[i] < thr_lin || sm[i] < sm[i - 1] || sm[i] < sm[i + 1])
            continue;
        bool dominant = true;
        for (int k = std::max(0, i - nms); k <= std::min(N - 1, i + nms); ++k)
            if (sm[k] > sm[i]) {
                dominant = false;
                break;
            }
        if (!dominant)
            continue;

        double wsum = 0.0, fnum = 0.0;
        float pk = -300.0f;
        for (int k = std::max(0, i - cw); k <= std::min(N - 1, i + cw); ++k) {
            wsum += lin[k];
            fnum += lin[k] * scan_detail::psd_freq_hz(
                                 static_cast<unsigned>(k), nfft,
                                 plan.sdr_rx_lo_hz, span_hz);
            pk = std::max(pk, psd_db[k]);
        }
        const double f = fnum / std::max(wsum, 1e-30);
        const long long snapped =
            static_cast<long long>(std::llround(f / 100000.0)) * 100000;
        if (snapped == last_snapped) {
            if (!out.empty() && pk > out.back().power_dbfs)
                out.back().power_dbfs = pk;
            continue;
        }
        last_snapped = snapped;

        bool configured = false;
        std::string label;
        for (const auto& st : plan.all_stations)
            if (std::llabs(st.freq_hz - snapped) <= 90000) {
                configured = true;
                label = st.label;
                break;
            }
        out.push_back({snapped, pk, configured, label});
        if (out.size() >= 128)
            break;
    }
    return out;
}

// The `GET /api/scan` body. `psd_db` is echoed in full (nfft values, ~4 kB
// at nfft=4096 after formatting) so the admin UI can draw the spectrum;
// `carriers` is the detector's output.
inline std::string scan_json(const Plan& plan, unsigned nfft,
                             const std::vector<float>& psd_db,
                             const std::vector<ScanCarrier>& carriers) {
    using scan_detail::fmt2;
    const double span_hz = static_cast<double>(plan.proc_rate_hz);

    std::string o = "{";
    o += "\"center_freq_hz\":" + std::to_string(plan.sdr_rx_lo_hz);
    o += ",\"samp_rate_hz\":" + std::to_string(plan.proc_rate_hz);
    o += ",\"nfft\":" + std::to_string(nfft);
    o += ",\"bin_width_hz\":" + fmt2(nfft ? span_hz / nfft : 0.0);

    double floor_db = 0.0;
    if (!psd_db.empty()) {
        std::vector<float> s(psd_db);
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        floor_db = s[s.size() / 2];
    }
    o += ",\"noise_floor_dbfs\":" + fmt2(floor_db);

    o += ",\"psd_dbfs\":[";
    for (size_t i = 0; i < psd_db.size(); ++i)
        o += (i ? "," : "") + fmt2(psd_db[i]);
    o += "]";

    o += ",\"carriers\":[";
    for (size_t i = 0; i < carriers.size(); ++i) {
        const ScanCarrier& c = carriers[i];
        o += (i ? "," : "");
        o += "{\"freq_hz\":" + std::to_string(c.freq_hz);
        o += ",\"power_dbfs\":" + fmt2(c.power_dbfs);
        o += ",\"configured\":" + std::string(c.configured ? "true" : "false");
        o += ",\"label\":" + http_detail::json_quote(c.label);
        o += "}";
    }
    o += "]}";
    return o;
}

} // namespace fmrx
