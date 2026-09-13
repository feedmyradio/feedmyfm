// Fixed worker pool that runs every MonoStation's per-block chain in
// parallel. Each worker owns a contiguous slice of the station list for
// the life of the pool (so a station's liquid filter state always touches
// the same thread / cache), and sleeps on a condition variable between
// blocks.
//
// Rolled by hand rather than using OpenMP: the surrounding loop has a long
// serial section (the channelizer), and OpenMP's default busy-wait
// (OMP_WAIT_POLICY=active, not settable from inside the process after
// libgomp init) makes idle workers spin through it and measurably slow it
// down. A condvar-blocked pool has no such effect and needs no env var.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fmrx {

class StationPool {
public:
    // work(k) processes station k. n = number of stations. want_threads > 0
    // forces the worker count; 0 auto-picks (hardware_concurrency, capped
    // at n). Fewer threads can actually be faster where condvar wakeups
    // are expensive -- e.g. inside a container.
    StationPool(int n, std::function<void(int)> work, int want_threads = 0)
        : m_work(std::move(work)), m_n(n) {
        int nthreads;
        if (want_threads > 0) {
            nthreads = want_threads;
        } else {
            unsigned hw = std::thread::hardware_concurrency();
            nthreads = static_cast<int>(hw ? hw : 4);
        }
        if (nthreads > n)
            nthreads = n;
        if (nthreads < 1)
            nthreads = 1;

        m_workers.reserve(nthreads);
        for (int t = 0; t < nthreads; ++t) {
            const int lo = static_cast<int>(static_cast<long>(t) * n / nthreads);
            const int hi =
                static_cast<int>(static_cast<long>(t + 1) * n / nthreads);
            m_workers.emplace_back([this, lo, hi] { worker(lo, hi); });
        }
    }

    ~StationPool() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
            ++m_gen;
        }
        m_cv.notify_all();
        for (auto& w : m_workers)
            w.join();
    }

    StationPool(const StationPool&) = delete;
    StationPool& operator=(const StationPool&) = delete;

    // Run all stations for one block; returns once every worker is done.
    void run_block() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_done = 0;
            ++m_gen;
        }
        m_cv.notify_all();
        std::unique_lock<std::mutex> lk(m_mtx);
        m_done_cv.wait(lk, [this] {
            return m_done == static_cast<int>(m_workers.size());
        });
    }

    int thread_count() const { return static_cast<int>(m_workers.size()); }

private:
    void worker(int lo, int hi) {
        unsigned long seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this, &seen] { return m_gen != seen || m_stop; });
                if (m_stop)
                    return;
                seen = m_gen;
            }
            for (int k = lo; k < hi; ++k)
                m_work(k);
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                ++m_done;
            }
            m_done_cv.notify_one();
        }
    }

    std::function<void(int)> m_work;
    int m_n;
    std::vector<std::thread> m_workers;

    std::mutex m_mtx;
    std::condition_variable m_cv;      // wakes workers for a new block
    std::condition_variable m_done_cv; // wakes run_block() when all done
    unsigned long m_gen = 0;
    int m_done = 0;
    bool m_stop = false;
};

} // namespace fmrx
