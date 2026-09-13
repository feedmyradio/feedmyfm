// Plain-libiio SDR source. Opens an AD9361-based SDR (PlutoSDR /
// FMComms2/3/4) over a libiio URI, configures the RX LO / sample rate /
// RF bandwidth / manual gain and the three tracking loops (RF DC, BB DC,
// quadrature), then hands out one block of complex<float> samples at a
// time, scaled to roughly +/-1.0.
//
// A dedicated reader thread runs iio_buffer_refill() back to back and
// converts into a small pool of buffers, so transport (which on its own
// sustains real time -- cf. `iio_readdev`) overlaps the DSP work instead
// of serialising behind it. If the consumer falls behind, the oldest
// queued block is dropped and counted rather than back-pressuring the SDR.
//
// Link loss (network blip to a remote Zynq SDR, device reboot) is handled
// in place: the reader tears down the IIO context, reconnects with
// backoff, and feeds silence downstream in the meantime so the DSP loop,
// the UDP streams and the webui stay alive. No process restart.
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <iio.h>

namespace fmrx {

using cfloat = std::complex<float>;

class PlutoSource {
public:
    PlutoSource(const std::string& uri, long long lo_hz, long long sample_rate_hz,
               long long rf_bandwidth_hz, int gain_db, size_t samples_per_block,
               std::string gain_mode = "manual")
        : m_block(samples_per_block),
          m_block_seconds(static_cast<double>(samples_per_block) /
                          static_cast<double>(sample_rate_hz)),
          m_uri(uri),
          m_lo_hz(lo_hz),
          m_sample_rate_hz(sample_rate_hz),
          m_rf_bandwidth_hz(rf_bandwidth_hz),
          m_gain_db(gain_db),
          m_gain_mode(std::move(gain_mode)),
          m_gain_cmd(static_cast<double>(gain_db)) {
        // Startup: retry briefly, then give up -- a persistent failure here
        // is almost always misconfig (bad URI) and should surface loudly.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        int backoff = 1;
        std::string last_err;
        while (!open_device(&last_err)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("iio: could not open SDR within 30s: " +
                                         last_err);
            std::fprintf(stderr, "sdr: open failed (%s), retrying in %ds\n",
                         last_err.c_str(), backoff);
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            backoff = std::min(backoff * 2, 8);
        }
        m_link_up.store(true);

        constexpr int kPoolDepth = 6;
        for (int i = 0; i < kPoolDepth; ++i)
            m_free.emplace_back(m_block);
        m_reader = std::thread([this] { reader_loop(); });
    }

    PlutoSource(const PlutoSource&) = delete;
    PlutoSource& operator=(const PlutoSource&) = delete;
    ~PlutoSource() {
        m_stop.store(true);
        m_cv.notify_all();
        if (m_reader.joinable())
            m_reader.join();
        cleanup();
    }

    size_t block_size() const { return m_block; }
    unsigned long overruns() const { return m_overruns.load(); }
    bool link_up() const { return m_link_up.load(); }
    const std::string& gain_mode() const { return m_gain_mode; }

    // Live front-end gain, read back from the AD9361 (the chip's own choice
    // under an AGC mode). NaN while the link is down or the phy channel is
    // momentarily contended -- callers cache the last good value. These are
    // attr reads over USB/network: call at most ~1 Hz from the status path,
    // never per block.
    double hardware_gain_db() const { return read_phy_attr("hardwaregain"); }
    double rssi_db() const { return read_phy_attr("rssi"); }

    // Software AGC: set the manual hardwaregain.
    // Clamped to the chip's 0..73 dB. No-op (and no error) while
    // reconnecting or if the phy channel is contended -- the caller's loop
    // re-issues on its next interval. Records the value so a reconnect
    // re-seeds to the last commanded gain, not the config ceiling.
    void set_hardware_gain(double g) {
        g = std::clamp(g, 0.0, 73.0);
        std::unique_lock<std::mutex> lk(m_phy_mtx, std::try_to_lock);
        if (!lk.owns_lock() || !m_rx_phy)
            return;
        if (iio_channel_attr_write_double(m_rx_phy, "hardwaregain", g) >= 0)
            m_gain_cmd.store(g);
    }

    // Blocks until the next block is available. Returns a pointer valid
    // until the following next_block() call.
    const cfloat* next_block() {
        std::unique_lock<std::mutex> lk(m_mtx);
        if (m_have_current) {
            m_free.push_back(std::move(m_current));
            m_have_current = false;
            m_cv.notify_all();
        }
        m_cv.wait(lk, [this] {
            return !m_ready.empty() || m_reader_err_set.load();
        });
        if (m_reader_err_set.load() && m_ready.empty())
            throw std::runtime_error(m_reader_err);
        m_current = std::move(m_ready.front());
        m_ready.pop_front();
        m_have_current = true;
        return m_current.data();
    }

private:
    // Open the IIO context and configure the device up to a ready-to-refill
    // buffer. Returns false (and cleans up) on any failure -- never throws.
    bool open_device(std::string* err_out = nullptr) {
        auto fail = [&](const std::string& msg) {
            if (err_out)
                *err_out = msg;
            cleanup();
            return false;
        };
        try {
            m_ctx = iio_create_context_from_uri(m_uri.c_str());
            if (!m_ctx)
                return fail("cannot open context '" + m_uri + "'");
            iio_context_set_timeout(m_ctx, 10000);

            m_phy = iio_context_find_device(m_ctx, "ad9361-phy");
            m_rx = iio_context_find_device(m_ctx, "cf-ad9361-lpc");
            if (!m_phy || !m_rx)
                return fail("ad9361-phy / cf-ad9361-lpc not found");

            iio_channel* rx_phy =
                iio_device_find_channel(m_phy, "voltage0", false);
            if (!rx_phy)
                return fail("phy RX channel voltage0 not found");
            wr_str(rx_phy, "rf_port_select", "A_BALANCED");
            wr_ll(rx_phy, "sampling_frequency", m_sample_rate_hz);
            wr_ll(rx_phy, "rf_bandwidth", m_rf_bandwidth_hz);
            // `auto_sw` keeps the chip in manual -- the software loop owns
            // the gain. The three real AD9361 AGC modes pass straight
            // through. hardwaregain is a hard write in the manual cases and
            // a non-fatal *seed* under a chip AGC mode (the driver rejects
            // it with -EBUSY once the AGC is running).
            const char* chip_mode =
                (m_gain_mode == "auto_sw") ? "manual" : m_gain_mode.c_str();
            wr_str(rx_phy, "gain_control_mode", chip_mode);
            if (std::strcmp(chip_mode, "manual") == 0)
                wr_dbl(rx_phy, "hardwaregain", m_gain_cmd.load());
            else
                try_wr_dbl(rx_phy, "hardwaregain", m_gain_cmd.load());
            try_wr_bool(rx_phy, "bb_dc_offset_tracking_en", true);
            try_wr_bool(rx_phy, "rf_dc_offset_tracking_en", true);
            try_wr_bool(rx_phy, "quadrature_tracking_en", true);

            iio_channel* lo =
                iio_device_find_channel(m_phy, "altvoltage0", true);
            if (!lo)
                return fail("RX LO channel altvoltage0 not found");
            wr_ll(lo, "frequency", m_lo_hz);

            m_ch_i = iio_device_find_channel(m_rx, "voltage0", false);
            m_ch_q = iio_device_find_channel(m_rx, "voltage1", false);
            if (!m_ch_i || !m_ch_q)
                return fail("cf-ad9361-lpc I/Q channels not found");
            iio_channel_enable(m_ch_i);
            iio_channel_enable(m_ch_q);

            iio_device_set_kernel_buffers_count(m_rx, 8);

            m_buf = iio_device_create_buffer(m_rx, m_block, false);
            if (!m_buf)
                return fail("iio_device_create_buffer failed (block too large / "
                            "device busy?)");

            // Retain the phy RX channel for the gain readback / software-AGC
            // write path. Published only now that the device is fully up;
            // cleanup() clears it under the same lock.
            {
                std::lock_guard<std::mutex> lk(m_phy_mtx);
                m_rx_phy = rx_phy;
            }
        } catch (const std::exception& e) {
            return fail(e.what());
        }
        return true;
    }

    // One AD9361 phy-channel attr read as a double. NaN on any failure
    // (link down, channel torn down mid-reconnect, contended lock, bad
    // attr). try_to_lock so a status-thread poll never blocks behind a
    // slow reconnect holding m_phy_mtx across open_device().
    double read_phy_attr(const char* attr) const {
        std::unique_lock<std::mutex> lk(m_phy_mtx, std::try_to_lock);
        if (!lk.owns_lock() || !m_rx_phy)
            return std::numeric_limits<double>::quiet_NaN();
        double v = 0.0;
        if (iio_channel_attr_read_double(m_rx_phy, attr, &v) < 0)
            return std::numeric_limits<double>::quiet_NaN();
        return v;
    }

    void reader_loop() {
        const char* fault_env = std::getenv("FMRX_SDR_FAULT_AFTER");
        long fault_after = fault_env ? std::atol(fault_env) : -1;
        long ok_blocks = 0;

        while (!m_stop.load()) {
            ssize_t nbytes;
            if (fault_after >= 0 && ok_blocks == fault_after) {
                nbytes = -ETIMEDOUT;
                fault_after = -1; // one-shot
                std::fprintf(stderr, "sdr: injecting fake fault "
                                     "(FMRX_SDR_FAULT_AFTER)\n");
            } else {
                nbytes = iio_buffer_refill(m_buf);
            }

            if (nbytes < 0) {
                char err[256];
                iio_strerror(static_cast<int>(-nbytes), err, sizeof(err));
                std::fprintf(stderr, "sdr: link lost (%s), reconnecting\n", err);
                m_link_up.store(false);
                cleanup();
                reconnect();
                if (m_stop.load())
                    return;
                ok_blocks = 0;
                continue;
            }
            ++ok_blocks;

            auto* p_i = static_cast<int16_t*>(iio_buffer_first(m_buf, m_ch_i));
            auto* p_q = static_cast<int16_t*>(iio_buffer_first(m_buf, m_ch_q));
            const ptrdiff_t step =
                iio_buffer_step(m_buf) / static_cast<ptrdiff_t>(sizeof(int16_t));

            std::vector<cfloat> buf = take_buffer();
            buf.resize(m_block);
            for (size_t k = 0; k < m_block; ++k) {
                // AD9361 delivers 12-bit signed samples; scale by 2048 to
                // land in +/-1.0.
                const float i =
                    static_cast<float>(p_i[static_cast<ptrdiff_t>(k) * step]) /
                    2048.0f;
                const float q =
                    static_cast<float>(p_q[static_cast<ptrdiff_t>(k) * step]) /
                    2048.0f;
                buf[k] = cfloat(i, q);
            }

            std::lock_guard<std::mutex> lk(m_mtx);
            m_ready.push_back(std::move(buf));
            m_cv.notify_all();
        }
    }

    // Retry open_device() forever (until m_stop), feeding one silence block
    // per real-time-block-interval so the downstream DSP loop / UDP / webui
    // never stall while the SDR is away.
    void reconnect() {
        int attempts = 0;
        auto next_try = std::chrono::steady_clock::now();
        while (!m_stop.load()) {
            std::vector<cfloat> buf = take_buffer();
            buf.assign(m_block, cfloat(0.0f, 0.0f));
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_ready.push_back(std::move(buf));
                m_cv.notify_all();
            }
            std::this_thread::sleep_for(
                std::chrono::duration<double>(m_block_seconds));

            if (std::chrono::steady_clock::now() < next_try)
                continue;
            std::string err;
            if (open_device(&err)) {
                m_link_up.store(true);
                std::fprintf(stderr, "sdr: reconnected after %d attempt(s)\n",
                             attempts + 1);
                return;
            }
            ++attempts;
            if (attempts <= 3 || attempts % 5 == 0)
                std::fprintf(stderr, "sdr: reconnect attempt %d failed (%s)\n",
                             attempts, err.c_str());
            next_try = std::chrono::steady_clock::now() +
                       std::chrono::seconds(attempts < 3 ? 1 : 5);
        }
    }

    // A recycled buffer if one is free, else a fresh one. Never blocks.
    std::vector<cfloat> take_buffer() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_free.empty()) {
            std::vector<cfloat> b = std::move(m_free.back());
            m_free.pop_back();
            return b;
        }
        if (!m_ready.empty()) {
            // Consumer is behind -- recycle the oldest queued block.
            std::vector<cfloat> b = std::move(m_ready.front());
            m_ready.pop_front();
            m_overruns.fetch_add(1);
            return b;
        }
        return std::vector<cfloat>(m_block);
    }

    void cleanup() {
        {
            std::lock_guard<std::mutex> lk(m_phy_mtx);
            m_rx_phy = nullptr; // points into m_ctx, destroyed just below
        }
        if (m_buf) iio_buffer_destroy(m_buf);
        if (m_ctx) iio_context_destroy(m_ctx);
        m_buf = nullptr;
        m_ctx = nullptr;
        m_phy = nullptr;
        m_rx = nullptr;
        m_ch_i = nullptr;
        m_ch_q = nullptr;
    }

    static void wr_str(iio_channel* c, const char* k, const char* v) {
        ssize_t r = iio_channel_attr_write(c, k, v);
        if (r < 0) throw_attr(k, r);
    }
    static void wr_ll(iio_channel* c, const char* k, long long v) {
        ssize_t r = iio_channel_attr_write_longlong(c, k, v);
        if (r < 0) throw_attr(k, r);
    }
    static void wr_dbl(iio_channel* c, const char* k, double v) {
        ssize_t r = iio_channel_attr_write_double(c, k, v);
        if (r < 0) throw_attr(k, r);
    }
    static void try_wr_bool(iio_channel* c, const char* k, bool v) {
        // Non-fatal: some FMComms revisions omit a tracking attr.
        iio_channel_attr_write_bool(c, k, v);
    }
    static void try_wr_dbl(iio_channel* c, const char* k, double v) {
        // Non-fatal: the AD9361 rejects hardwaregain with -EBUSY once a
        // chip AGC mode is active -- the write is only a seed there.
        iio_channel_attr_write_double(c, k, v);
    }
    [[noreturn]] static void throw_attr(const char* k, ssize_t r) {
        char err[256];
        iio_strerror(static_cast<int>(-r), err, sizeof(err));
        throw std::runtime_error(std::string("writing '") + k + "': " + err);
    }

    size_t m_block;
    double m_block_seconds;
    std::string m_uri;
    long long m_lo_hz;
    long long m_sample_rate_hz;
    long long m_rf_bandwidth_hz;
    int m_gain_db;                    // config sdr.gain_db (startup / ceiling)
    std::string m_gain_mode;         // config sdr.gain_mode
    std::atomic<double> m_gain_cmd;  // last commanded manual gain; re-seeded on reconnect

    iio_context* m_ctx = nullptr;
    iio_device* m_phy = nullptr;
    iio_device* m_rx = nullptr;
    iio_channel* m_ch_i = nullptr;
    iio_channel* m_ch_q = nullptr;
    iio_channel* m_rx_phy = nullptr; // ad9361-phy voltage0, retained; guarded by m_phy_mtx
    iio_buffer* m_buf = nullptr;
    mutable std::mutex m_phy_mtx;    // serialises m_rx_phy lifetime + attr IO

    std::thread m_reader;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_link_up{false};
    std::atomic<unsigned long> m_overruns{0};
    std::atomic<bool> m_reader_err_set{false};
    std::string m_reader_err;

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::vector<cfloat>> m_free;
    std::deque<std::vector<cfloat>> m_ready;
    std::vector<cfloat> m_current;
    bool m_have_current = false;
};

} // namespace fmrx
