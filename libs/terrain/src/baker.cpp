#include <hog/core/profile.hpp>
#include <hog/terrain/baker.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace hog::terrain {

void PatchBaker::start(planet::CellId cell) {
    baking_.push_back(
        std::make_unique<jobs::Async<Patch>>(*jobs_, [world = world_, cell, fates = fates_] {
            HOG_ZONE("bake patch");
            return bake_patch(*world, cell, *fates);
        }));
}

std::vector<Patch> PatchBaker::collect() {
    std::vector<Patch> baked;
    std::erase_if(baking_, [&](const std::unique_ptr<jobs::Async<Patch>>& bake) {
        if (!bake->done()) {
            return false;
        }
        baked.push_back(bake->take());
        return true;
    });
    return baked;
}

std::vector<Patch> PatchBaker::finish() {
    std::vector<Patch> baked;
    baked.reserve(baking_.size());
    for (const auto& bake : baking_) {
        baked.push_back(bake->take());
    }
    baking_.clear();
    return baked;
}

} // namespace hog::terrain
