#include <hog/flow/simulation.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace hog::flow {

using globe::BrickSite;

Simulation::Simulation(globe::World& world) : world_{&world}, region_{world} {}

void Simulation::take(BrickSite site) {
    const globe::VoxelStore& store = world_->store();
    if (!store.holds_site(site)) {
        return;
    }
    region_.load(site);
    still_[globe::brick_key(site)] = 0;
}

void Simulation::touched(BrickSite site) {
    take(site);
    constexpr std::array<std::array<int, 3>, 6> about{
        {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
    for (const auto& [di, dj, dl] : about) {
        const std::int64_t i = std::int64_t{site.i} + di;
        const std::int64_t j = std::int64_t{site.j} + dj;
        const std::int64_t l = std::int64_t{site.layer} + dl;
        if (i >= 0 && j >= 0 && l >= 0) {
            take({.face = site.face,
                  .i = static_cast<std::uint32_t>(i),
                  .j = static_cast<std::uint32_t>(j),
                  .layer = static_cast<std::uint32_t>(l)});
        }
    }
}

Simulation::Progress Simulation::step() {
    Progress progress;
    if (region_.size() == 0) {
        return progress;
    }
    const Stepped stepped = region_.step();
    progress.changed = stepped.changed;
    for (std::size_t k = 0; k < std::min(stepped.wanted.size(), most_taken); ++k) {
        take(stepped.wanted[k]);
    }
    progress.written = region_.flush();
    for (const BrickSite brick : progress.written) {
        still_[globe::brick_key(brick)] = 0;
    }
    std::vector<BrickSite> let_go;
    for (auto& [key, steps] : still_) {
        if (++steps > rest) {
            let_go.push_back(globe::brick_at_key(key));
        }
    }
    for (const BrickSite brick : let_go) {
        region_.unload(brick);
        still_.erase(globe::brick_key(brick));
    }
    progress.held = region_.size();
    return progress;
}

} // namespace hog::flow
