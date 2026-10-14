#pragma once

#include "fumar/core/types.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fumar {

/// A handful of threads that split one loop between them.
///
/// The only threading in the engine, and deliberately the simplest kind there
/// is: no queue of independent jobs, no dependencies, no futures. One loop at a
/// time is cut into chunks, every thread - the caller included - takes chunks
/// until none are left, and parallelFor returns when the last one is done. That
/// is all the CPU workload needs, and it is easy to see that it is correct.
///
/// Chunks are handed out by one atomic counter rather than divided up front,
/// so a thread that drew cheap chunks simply takes more of them: nobody waits
/// on the one that drew the slow ones.
///
/// One parallelFor at a time, from one thread. Calling it from inside `fn` -
/// a loop within a loop - is not supported.
class JobPool {
public:
    /// `workers` threads besides the caller; 0 picks one fewer than the
    /// machine has hardware threads, since the caller is the last one.
    explicit JobPool(u32 workers = 0);
    ~JobPool();

    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    /// Threads a loop is split between, the caller included.
    u32 threadCount() const { return static_cast<u32>(m_threads.size()) + 1; }

    /// Calls fn(begin, end) over [0, count) in pieces of at most `chunk`,
    /// on every thread at once, and returns when all of it has run.
    void parallelFor(usize count, usize chunk, const std::function<void(usize, usize)>& fn);

private:
    struct Job {
        const std::function<void(usize, usize)>* fn = nullptr;
        usize count = 0;
        usize chunk = 1;
        std::atomic<usize> next{0};
    };

    void workerLoop();
    static void drain(Job& job);

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;

    Job m_job;
    bool m_hasJob = false;
    u64 m_generation = 0;
    u32 m_active = 0;
    bool m_stop = false;
};

} // namespace fumar
