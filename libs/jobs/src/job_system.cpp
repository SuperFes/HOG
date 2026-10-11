#include <hog/jobs/job_system.hpp>

namespace hog::jobs {

JobSystem::JobSystem(std::uint32_t thread_count) {
    if (thread_count == 0) {
        scheduler_.Initialize();
    } else {
        scheduler_.Initialize(thread_count);
    }
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
