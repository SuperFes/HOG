#pragma once

#include "fine_planet.hpp"

#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Hand-made ground for testing rules on: small patches of voxels written where wanted, and
// measured, with no planet made for them.
namespace hog::globe::bed {

using hog::DVec3;

using hog::globe::Cell;
using hog::globe::Generator;
using hog::globe::MadeWorld;
using hog::globe::Material;
using hog::globe::World;

inline const Generator& earth() {
    static const Generator generator{fine(hog::globe::Planet{})};
    return generator;
}

inline DVec3 somewhere(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
}

// A direction whose ground stands between two heights over the radius, rising gently, with no
// cave within a chunk's width under it, so a chunk there holds the ground alone.
inline DVec3 ground_between(const Generator& generator, double low, double high) {
    const hog::globe::Layers& layers = generator.layers();
    for (std::uint64_t k = 0;; ++k) {
        const DVec3 direction = somewhere(k);
        const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0) -
                              generator.planet().radius;
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const DVec3 north = glm::cross(direction, east);
        bool gentle = true;
        for (const DVec3 side : {east, -east, north, -north}) {
            const DVec3 across =
                glm::normalize(direction + (side * 24.0 / generator.planet().radius));
            gentle =
                gentle && std::abs(hog::globe::ground_below(generator, across, 0.5).value_or(0.0) -
                                   generator.planet().radius - ground) < 8.0;
        }
        if (gentle && ground > low && ground < high &&
            generator.column(direction, 0.5)
                .caves(layers.bottom(0), layers.top(layers.count - 1), 16.0)
                .empty()) {
            return direction;
        }
    }
}

// Changes each open voxel within `radius` of `centre`, or every voxel there with `ground` too,
// given it and the radius its bottom stands at.
inline void fill_hollow(World& world, DVec3 centre, double radius, const auto& change,
                        bool ground = false) {
    const hog::globe::Layers& layers = world.layers();
    const Cell middle = hog::globe::cell_under(glm::normalize(centre), layers.level);
    const int layer = layers.layer_at(glm::length(centre));
    const int reach = static_cast<int>(std::ceil(radius / layers.thickness)) + 1;
    std::map<hog::globe::BrickSite, hog::globe::BrickVoxels> bricks;
    for (int dl = -reach; dl <= reach; ++dl) {
        for (int dj = -reach; dj <= reach; ++dj) {
            for (int di = -reach; di <= reach; ++di) {
                const std::optional<Cell> cell = hog::globe::next_to(middle, di, dj);
                if (!cell) {
                    continue;
                }
                const hog::globe::Site site{.face = cell->face,
                                            .i = cell->i,
                                            .j = cell->j,
                                            .layer = static_cast<std::uint32_t>(layer + dl)};
                if (glm::length(hog::globe::middle_of(site, layers) - centre) > radius) {
                    continue;
                }
                const hog::globe::BrickSite brick = hog::globe::brick_of(site);
                auto held = bricks.find(brick);
                if (held == bricks.end()) {
                    held = bricks.emplace(brick, world.brick(brick)).first;
                }
                hog::globe::Voxel& voxel =
                    held->second[static_cast<std::size_t>(hog::globe::index_in_brick(site))];
                if (ground || voxel.solid == Material::Air) {
                    change(voxel, layers.bottom(static_cast<int>(site.layer)));
                }
            }
        }
    }
    for (const auto& [brick, voxels] : bricks) {
        world.write(brick, voxels);
    }
}


} // namespace hog::globe::bed
