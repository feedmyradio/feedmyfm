// Which stations currently have a live listener, as pushed by webui's
// relay via `POST /active`. The RT loop consults this once per block to
// gate each station's per-block decode chain (see main.cpp run_live) --
// a station with no listener costs ~0.
//
// Only used when config `decode.listener_gated` is true. With the toggle
// off (the default) the object is never built and every station decodes
// unconditionally, exactly as before this feature existed.
//
// Fail-open by construction:
//   * initial state is all-on -- until the first POST lands, nothing is
//     gated;
//   * if the last POST is older than kFailOpenMs the effective set snaps
//     back to all-on.
// A dead, wedged or disconnected relay therefore can never silence a
// station; the cost of that failure mode is that we keep decoding
// everything, i.e. today's load.
//
// Station identity here is the plan-order index (0..n-1). The HTTP
// handler maps the POSTed UDP ports to indices before calling post().
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace fmrx {

class ActiveSet {
public:
    // A relay keepalive is expected every ~5 s; 30 s is 6 missed in a row.
    static constexpr std::int64_t kFailOpenMs = 30'000;

    // 64 station indices fit one word. Above that the bitset approach
    // needs widening -- callers disable gating instead (main.cpp).
    static constexpr int kMaxStations = 64;

    explicit ActiveSet(int n) : m_n(n) {}

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // HTTP thread. `mask` bit k set => station k has a listener. Bits at
    // or above m_n are ignored by effective().
    void post(std::uint64_t mask) {
        m_mask.store(mask, std::memory_order_relaxed);
        m_last_post_ms.store(now_ms(), std::memory_order_relaxed);
    }

    // The set in force right now: the posted mask while a recent POST
    // stands, otherwise all-on (never posted, or stale => fail open).
    std::uint64_t effective(std::int64_t now) const {
        const std::int64_t last =
            m_last_post_ms.load(std::memory_order_relaxed);
        if (last == 0 || now - last > kFailOpenMs)
            return all_on();
        return m_mask.load(std::memory_order_relaxed) & all_on();
    }

    // True while relay control is actually in effect (a fresh POST
    // stands). False before the first POST and after a fail-open lapse --
    // status/telemetry uses this to explain why nothing looks gated.
    bool controlled(std::int64_t now) const {
        const std::int64_t last =
            m_last_post_ms.load(std::memory_order_relaxed);
        return last != 0 && now - last <= kFailOpenMs;
    }

    std::uint64_t all_on() const {
        return m_n >= kMaxStations
                   ? ~std::uint64_t(0)
                   : (std::uint64_t(1) << m_n) - 1;
    }

    int size() const { return m_n; }

private:
    int m_n;
    std::atomic<std::uint64_t> m_mask{~std::uint64_t(0)};
    std::atomic<std::int64_t> m_last_post_ms{0};
};

} // namespace fmrx
