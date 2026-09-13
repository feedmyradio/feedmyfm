// Streaming averaged periodogram over the SDR's wideband input, for the
// passive band scan served on `GET /api/scan`. Thin wrapper over liquid's
// spgramcf.
//
// Why not just read channelizer bin power: the channelizer's bins are
// ~one bin_width apart (256 kHz for the 16.384 MHz / 64-ch baseline) and
// each bin's slice is `oversample`x wider still -- far too coarse to see
// where a carrier sits, or that two stations share a slice. A real FFT at
// nfft = 4096 over the same span is ~4 kHz resolution.
//
// Cost: one nfft-point FFT per nfft input samples (hop == nfft, no
// overlap). The receiver only feeds this while a scan request is pending,
// so the steady-state cost is a single branch per block.
//
// NOT thread-safe. The RT loop owns the object: it calls reset() / write()
// / get_psd() and publishes a copy of the PSD to the HTTP handler under
// its own lock.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include <liquid/liquid.h>

namespace fmrx {

using cfloat = std::complex<float>; // == liquid_float_complex under C++

class Spectrum {
public:
    // nfft: FFT size (power of two). samp_rate_hz / center_hz: the span the
    // PSD covers, for bin_freq_hz().
    Spectrum(unsigned nfft, double samp_rate_hz, long long center_hz)
        : m_nfft(nfft), m_fs(samp_rate_hz), m_center(center_hz) {
        // Hann window, full length, hop == nfft. alpha = -1 => accumulate
        // every transform since the last reset() with equal weight, i.e. a
        // plain average over the scan window.
        m_q = spgramcf_create(nfft, LIQUID_WINDOW_HANN, nfft, nfft);
        if (!m_q)
            throw std::runtime_error("spgramcf_create failed");
        spgramcf_set_alpha(m_q, -1.0f);
    }

    ~Spectrum() {
        if (m_q)
            spgramcf_destroy(m_q);
    }

    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;

    unsigned nfft() const { return m_nfft; }

    // Drop all accumulated transforms -- call once at the start of a scan.
    void reset() { spgramcf_reset(m_q); }

    void write(const cfloat* x, std::size_t n) {
        spgramcf_write(m_q, const_cast<cfloat*>(x), static_cast<unsigned>(n));
    }

    // Fill psd_db (resized to nfft) with the fft-shifted PSD in dB:
    //   psd_db[0]        == power at center - fs/2
    //   psd_db[nfft/2]   == power at center (DC)
    //   psd_db[nfft-1]   == power at center + fs/2 - fs/nfft
    void get_psd(std::vector<float>& psd_db) const {
        psd_db.resize(m_nfft);
        spgramcf_get_psd(m_q, psd_db.data());
    }

    // RF centre frequency of PSD sample i, matching the fft-shifted layout
    // above.
    long long bin_freq_hz(unsigned i) const {
        return m_center +
               static_cast<long long>(std::llround(
                   (static_cast<double>(i) / m_nfft - 0.5) * m_fs));
    }

private:
    unsigned m_nfft;
    double m_fs;
    long long m_center;
    spgramcf m_q = nullptr;
};

} // namespace fmrx
