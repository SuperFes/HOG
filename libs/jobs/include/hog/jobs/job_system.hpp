#pragma once

#include <hog/core/assert.hpp>

#include <TaskScheduler.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace hog::jobs {

// Half-open range of item indices, [begin, end).
struct Range {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

template <typename T>
class Async;

// Worker pool backed by enkiTS. The thread that creates it counts as thread 0 and runs work while
// it waits, so a pool of N threads starts N - 1 workers.
class JobSystem {
public:
    // 0 means one thread per hardware thread.
    explicit JobSystem(std::uint32_t thread_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    [[nodiscard]] std::uint32_t thread_count() const noexcept;

    // Calls body(range, thread_index) over [0, count) in chunks of at least min_chunk items and
    // returns once all of them have run; thread_index is below thread_count(). Call it from the
    // creating thread or from inside a job. The first exception thrown by body is rethrown here,
    // and chunks that had not started by then are skipped.
    template <typename F>
        requires std::invocable<F&, Range, std::uint32_t>
    void parallel_for(std::uint32_t count, std::uint32_t min_chunk, F body);

private:
    template <typename F>
    class ForTask;
    template <typename T>
    friend class Async;

    [[nodiscard]] bool on_pool_thread() const noexcept;

    enki::TaskScheduler scheduler_;
};

template <typename F>
class JobSystem::ForTask final : public enki::ITaskSet {
public:
    ForTask(std::uint32_t count, std::uint32_t min_chunk, F& body)
        : ITaskSet(count, min_chunk), body_(&body) {}

    void ExecuteRange(enki::TaskSetPartition range, std::uint32_t thread) override {
        if (failed_.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            (*body_)(Range{.begin = range.start, .end = range.end}, thread);
        } catch (...) {
            // Only the first thrower writes error_; enkiTS's completion orders it before the wait.
            if (!failed_.exchange(true, std::memory_order_relaxed)) {
                error_ = std::current_exception();
            }
        }
    }

    void rethrow_if_failed() const {
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    F* body_;
    std::atomic<bool> failed_{false};
    std::exception_ptr error_;
};

template <typename F>
    requires std::invocable<F&, Range, std::uint32_t>
void JobSystem::parallel_for(std::uint32_t count, std::uint32_t min_chunk, F body) {
    HOG_ASSERT(min_chunk > 0, "a chunk must hold at least one item");
    HOG_ASSERT(on_pool_thread(), "parallel_for called from a thread outside the pool");
    if (count == 0) {
        return;
    }
    ForTask<F> task(count, min_chunk, body);
    scheduler_.AddTaskSetToPipe(&task);
    // Leaves background work to the workers rather than stalling on it here.
    scheduler_.WaitforTask(&task, enki::TASK_PRIORITY_HIGH);
    task.rethrow_if_failed();
}

// A result worked out on the pool in the background, behind work started with parallel_for. It
// starts once made, from the creating thread or a job, and runs on the workers; with no workers it
// runs only once waited on. It mustn't outlive its JobSystem, and destroying it waits for it.
template <typename T>
class Async final : enki::ITaskSet {
public:
    template <typename F>
        requires std::invocable<F&> && std::convertible_to<std::invoke_result_t<F&>, T>
    Async(JobSystem& jobs, F body) : jobs_(&jobs), body_(std::move(body)) {
        HOG_ASSERT(jobs.on_pool_thread(), "Async started from a thread outside the pool");
        m_Priority = enki::TASK_PRIORITY_LOW;
        jobs.scheduler_.AddTaskSetToPipe(this);
    }
    ~Async() override { wait(); }

    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;
    Async(Async&&) = delete;
    Async& operator=(Async&&) = delete;

    [[nodiscard]] bool done() const noexcept { return GetIsComplete(); }

    // Waits for it, then hands over what it returned or rethrows what it threw. Only once.
    [[nodiscard]] T take() {
        wait();
        HOG_ASSERT(!taken_, "an Async's result taken twice");
        taken_ = true;
        // Nothing's returned only if it threw.
        if (!result_) {
            std::rethrow_exception(error_);
        }
        return std::move(*result_);
    }

private:
    void ExecuteRange(enki::TaskSetPartition /*range*/, std::uint32_t /*thread*/) override {
        try {
            result_.emplace(body_());
        } catch (...) {
            error_ = std::current_exception();
        }
    }

    void wait() {
        if (!done()) {
            HOG_ASSERT(jobs_->on_pool_thread(), "Async waited on from a thread outside the pool");
            jobs_->scheduler_.WaitforTask(this);
        }
    }

    JobSystem* jobs_;
    std::function<T()> body_;
    std::optional<T> result_;
    std::exception_ptr error_;
    bool taken_ = false;
};

} // namespace hog::jobs
