#pragma once

#include <hog/jobs/job_system.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/world.hpp>
#include <hog/terrain/patch.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace hog::terrain {

// Bakes patches in the background on the pool, for the thread that made it. The world mustn't
// change while any are in flight; finish() them first.
class PatchBaker {
public:
    PatchBaker(const planet::World& world, jobs::JobSystem& jobs) noexcept
        : world_(&world), jobs_(&jobs) {}

    // Bakes started from now on read the woods' fates from these. Those in flight keep theirs.
    void fates(std::shared_ptr<const flora::Fates> fates) noexcept { fates_ = std::move(fates); }

    void start(planet::CellId cell);
    [[nodiscard]] std::size_t in_flight() const noexcept { return baking_.size(); }

    // The bakes done since the last call.
    [[nodiscard]] std::vector<Patch> collect();
    // Waits for every bake still in flight, and hands them all over.
    [[nodiscard]] std::vector<Patch> finish();

private:
    const planet::World* world_;
    jobs::JobSystem* jobs_;
    std::shared_ptr<const flora::Fates> fates_ = std::make_shared<const flora::Fates>();
    std::vector<std::unique_ptr<jobs::Async<Patch>>> baking_;
};

} // namespace hog::terrain
