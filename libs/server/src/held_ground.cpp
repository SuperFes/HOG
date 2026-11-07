#include <hog/globe/cell.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/held_ground.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hog::server {
namespace {

constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);

void sort_unique(std::vector<globe::Site>& corners) {
    std::ranges::sort(corners);
    const auto [first, last] = std::ranges::unique(corners);
    corners.erase(first, last);
}

} // namespace

HeldGround::HeldGround(globe::Volume& volume, globe::VolumeArchive& archive,
                       globe::VolumeOrigin origin)
    : volume_{&volume}, archive_{&archive}, origin_{origin} {}

std::vector<globe::Site> HeldGround::under(std::span<const DVec3> places) const {
    std::vector<globe::Site> corners;
    corners.reserve(places.size());
    for (const DVec3 place : places) {
        const globe::Cell cell = globe::cell_under(glm::normalize(place), origin_.layers.level);
        corners.push_back(
            {.face = cell.face, .i = cell.i / side * side, .j = cell.j / side * side});
    }
    std::ranges::sort(corners);
    return corners;
}

std::vector<globe::Site> HeldGround::near(std::span<const DVec3> places, double metres) const {
    std::vector<globe::Site> corners;
    for (const DVec3 place : places) {
        const std::vector<globe::Site> here =
            globe::tile_columns_near(origin_.layers, place, metres);
        corners.insert(corners.end(), here.begin(), here.end());
    }
    sort_unique(corners);
    return corners;
}

bool HeldGround::moved(std::span<const DVec3> places) const {
    return under(places) != under_;
}

Result<HeldGround::Changed> HeldGround::keep(std::span<const DVec3> places,
                                             std::span<const globe::Site> pinned) {
    Changed changed;
    auto loaded = globe::load_columns(*volume_, *archive_, near(places, reach));
    if (!loaded) {
        return std::unexpected(std::move(loaded).error());
    }
    changed.loaded = std::move(*loaded);

    const std::vector<globe::Site> kept = near(places, reach + slack);
    for (const globe::Site corner : volume_->columns()) {
        if (!std::ranges::binary_search(kept, corner) &&
            !std::ranges::binary_search(pinned, corner)) {
            changed.dropped.push_back(corner);
        }
    }
    if (auto saved = globe::save_columns(*volume_, *archive_, origin_, changed.dropped); !saved) {
        return std::unexpected(std::move(saved).error());
    }
    for (const globe::Site corner : changed.dropped) {
        volume_->drop_column(corner);
    }
    under_ = under(places);
    return changed;
}

std::vector<globe::Site> HeldGround::corners_of(std::span<const globe::BrickSite> bricks) {
    constexpr std::uint32_t across = side / globe::brick_side;
    std::vector<globe::Site> corners;
    corners.reserve(bricks.size());
    for (const globe::BrickSite brick : bricks) {
        corners.push_back(
            {.face = brick.face, .i = brick.i / across * side, .j = brick.j / across * side});
    }
    sort_unique(corners);
    return corners;
}

} // namespace hog::server
