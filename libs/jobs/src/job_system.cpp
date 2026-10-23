#include <hog/core/profile.hpp>
#include <hog/jobs/job_system.hpp>

#include <format>

namespace hog::jobs {

#ifdef HOG_PROFILE
namespace {

void name_worker(std::uint32_t thread) {
    HOG_THREAD(std::format("worker {}", thread).c_str());
}

} // namespace
#endif

JobSystem::JobSystem(std::uint32_t thread_count) {
    enki::TaskSchedulerConfig config;
    if (thread_count != 0) {
        config.numTaskThreadsToCreate = thread_count - 1;
    }
#ifdef HOG_PROFILE
    config.profilerCallbacks.threadStart = name_worker;
#endif
    scheduler_.Initialize(config);
}

JobSystem::~JobSystem() {
    scheduler_.WaitforAllAndShutdown();
}

std::uint32_t JobSystem::thread_count() const noexcept {
    return scheduler_.GetNumTaskThreads();
}

bool JobSystem::on_pool_thread() const noexcept {
    return scheduler_.GetThreadNum() != enki::NO_THREAD_NUM;
}

} // namespace hog::jobs
