// Thin wrapper over liquid's rational-rate polyphase channelizer
// (firpfbchr_crcf): splits the proc_rate input into `num_channels` bins,
// each coming out at proc_rate / decim_p  (== channel_rate; decim_p =
// num_channels / oversample, so oversample=2 => each bin runs at 2x
// critical rate).
//
// Output bin ordering is FFT-bin order (index 0 == DC, 1..M/2-1 positive
// frequencies, M/2..M-1 negative) -- the same convention the plan's `bin`
// index uses (see resolve.hpp), so it is applied directly.
#pragma once

#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

#include <liquid/liquid.h>

namespace fmrx {

using cfloat = std::complex<float>;

class Channelizer {
public:
    Channelizer(int num_channels, int decim_p, int proto_semilen_m,
                float atten_db)
        : m_M(num_channels), m_P(decim_p) {
        m_q = firpfbchr_crcf_create_kaiser(
            static_cast<unsigned int>(m_M), static_cast<unsigned int>(m_P),
            static_cast<unsigned int>(proto_semilen_m), atten_db);
        if (!m_q)
            throw std::runtime_error("firpfbchr_crcf_create_kaiser failed");
        m_scratch.resize(m_M);
        m_bins.resize(m_M);
        m_all.resize(m_M);
        for (int k = 0; k < m_M; ++k)
            m_all[k] = k;
        m_norm = 1.0f / measure_center_bin_gain(proto_semilen_m, atten_db);
    }

    Channelizer(const Channelizer&) = delete;
    Channelizer& operator=(const Channelizer&) = delete;
    ~Channelizer() {
        if (m_q)
            firpfbchr_crcf_destroy(m_q);
    }

    int channels() const { return m_M; }
    int decim() const { return m_P; }

    // Restrict the per-block scatter to the bins that actually feed a
    // station. firpfbchr_crcf_execute still computes all M internally, but
    // copying only the ~15 live bins out of 64 (and applying the gain
    // normalisation) is a meaningful chunk of the per-block cost.
    void set_active_bins(std::vector<int> bins) { m_active = std::move(bins); }

    // Channelize one input block. n_in must be a multiple of decim_p.
    // After the call, bin(k) has n_in / decim_p samples at channel_rate
    // for every k in the active-bin set (all M bins if none was set).
    void channelize(const cfloat* in, size_t n_in) {
        if (n_in % static_cast<size_t>(m_P) != 0)
            throw std::runtime_error(
                "Channelizer: input block not a multiple of decim_p");
        const size_t n_out = n_in / static_cast<size_t>(m_P);

        const std::vector<int>& active = m_active.empty() ? m_all : m_active;
        for (int k : active)
            m_bins[k].resize(n_out);

        size_t t = 0;
        for (size_t off = 0; off < n_in; off += static_cast<size_t>(m_P), ++t) {
            firpfbchr_crcf_push(m_q, const_cast<cfloat*>(in + off));
            firpfbchr_crcf_execute(m_q, m_scratch.data());
            for (int k : active)
                m_bins[k][t] = m_norm * m_scratch[k];
        }
    }

    const std::vector<cfloat>& bin(int k) const { return m_bins.at(k); }

    // Passband gain the raw filterbank applies at a bin centre (before the
    // 1/gain normalisation channelize() bakes in). liquid's Kaiser
    // prototype is not unity-normalised and the oversampled bank adds a
    // further M/P, so this is ~5-6x for the 64ch/oversample-2 setup.
    float raw_center_gain() const { return 1.0f / m_norm; }

private:
    // Push a unit CW tone sitting exactly on bin 1's centre through a
    // throwaway channelizer with identical parameters and return the
    // steady-state output magnitude in that bin.
    float measure_center_bin_gain(int proto_semilen_m, float atten_db) const {
        firpfbchr_crcf probe = firpfbchr_crcf_create_kaiser(
            static_cast<unsigned int>(m_M), static_cast<unsigned int>(m_P),
            static_cast<unsigned int>(proto_semilen_m), atten_db);
        std::vector<cfloat> in(static_cast<size_t>(m_P));
        std::vector<cfloat> out(static_cast<size_t>(m_M));
        const double w = 2.0 * M_PI * 1.0 / m_M; // bin 1 centre, cycles/sample
        const int warmup = 4 * proto_semilen_m + 8;
        const int measure = 64;
        double acc = 0.0;
        long nidx = 0;
        for (int blk = 0; blk < warmup + measure; ++blk) {
            for (int j = 0; j < m_P; ++j, ++nidx)
                in[j] = cfloat(static_cast<float>(std::cos(w * nidx)),
                               static_cast<float>(std::sin(w * nidx)));
            firpfbchr_crcf_push(probe, in.data());
            firpfbchr_crcf_execute(probe, out.data());
            if (blk >= warmup)
                acc += std::abs(out[1]);
        }
        firpfbchr_crcf_destroy(probe);
        float g = static_cast<float>(acc / measure);
        return g > 1e-6f ? g : 1.0f;
    }

    int m_M, m_P;
    firpfbchr_crcf m_q = nullptr;
    float m_norm = 1.0f;
    std::vector<cfloat> m_scratch;
    std::vector<std::vector<cfloat>> m_bins;
    std::vector<int> m_all;    // {0..M-1}
    std::vector<int> m_active; // subset actually consumed; empty => all
};

} // namespace fmrx
