#include "fumar/core/job_pool.hpp"

#include <algorithm>

namespace fumar {

JobPool::JobPool(u32 workers) {
    if (workers == 0) {
        const u32 hardware = std::thread::hardware_concurrency();
        workers = hardware > 1 ? hardware - 1 : 0;
    }
    m_threads.reserve(workers);
    for (u32 i = 0; i < workers; ++i) {
        m_threads.emplace_back([this] { workerLoop(); });
    }
}

JobPool::~JobPool() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

void JobPool::drain(Job& job) {
    for (;;) {
        const usize begin = job.next.fetch_add(job.chunk, std::memory_order_relaxed);
        if (begin >= job.count) {
            return;
        }
        (*job.fn)(begin, std::min(begin + job.chunk, job.count));
    }
}

void JobPool::parallelFor(usize count, usize chunk, const std::function<void(usize, usize)>& fn) {
    if (count == 0) {
        return;
    }
    chunk = std::max<usize>(chunk, 1);

    // Not worth waking anyone for: one chunk, or nobody to wake.
    if (m_threads.empty() || count <= chunk) {
        fn(0, count);
        return;
    }

    {
        const std::scoped_lock lock(m_mutex);
        m_job.fn = &fn;
        m_job.count = count;
        m_job.chunk = chunk;
        m_job.next.store(0, std::memory_order_relaxed);
        m_hasJob = true;
        ++m_generation;
    }
    m_wake.notify_all();

    drain(m_job);

    // Every chunk has been TAKEN once drain returns, but a worker may still be
    // running the last one it took. `fn` lives on the caller's stack, so the
    // job is withdrawn only once nobody is inside it. A worker that wakes after
    // this finds no job and goes back to sleep.
    std::unique_lock lock(m_mutex);
    m_done.wait(lock, [this] { return m_active == 0; });
    m_hasJob = false;
    m_job.fn = nullptr;
}

void JobPool::workerLoop() {
    u64 seen = 0;
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
        if (m_stop) {
            return;
        }
        seen = m_generation;
        if (!m_hasJob) {
            continue;
        }

        ++m_active;
        lock.unlock();
        drain(m_job);
        lock.lock();
        if (--m_active == 0) {
            m_done.notify_all();
        }
    }
}

} // namespace fumar
