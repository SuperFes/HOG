#include <hog/core/assert.hpp>
#include <hog/flow/region.hpp>
#include <hog/globe/generator.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::flow {
namespace {

// A rock that has lost its hold is looked at again for this many steps while the bricks about it
// are taken up.
constexpr int most_tries = 20;
// The share of a boulder's radius that lies under the ground it comes to rest on.
constexpr double settles_in = 0.4;

// A rock's volume as a sphere's radius, which is what it rolls on as.
double rolling_radius(const globe::Rock& rock) {
    return std::cbrt(rock.axes.x * rock.axes.y * rock.axes.z);
}

} // namespace

std::optional<globe::BrickSite> Region::reshape(DVec3 centre, double reach,
                                                const Distance& distance, const Change& change) {
    const globe::Layers& layers = world_->store().layers();
    const double band = globe::distance_band(layers);
    std::optional<globe::BrickSite> missing;
    for (const auto& [brick, indices] : globe::sites_near(layers, centre, reach + band)) {
        Held* in = held(brick);
        if (in == nullptr) {
            missing = missing.value_or(brick);
            continue;
        }
        for (const int index : indices) {
            DVec3 outward{0.0};
            const double to = distance(globe::middle_of(globe::site_in(brick, index), layers),
                                       outward);
            if (to <= band) {
                change(in->voxels[static_cast<std::size_t>(index)], std::max(to, -band), outward);
                in->dirty = true;
            }
        }
    }
    return missing;
}

std::optional<bool> Region::solid_at(DVec3 point) {
    const globe::Layers& layers = world_->store().layers();
    const double radius = glm::length(point);
    const int layer = layers.layer_at(radius);
    if (layer < 0 || layer >= layers.count) {
        return false;
    }
    const globe::Cell column = globe::cell_under(point / radius, layers.level);
    const globe::Voxel* here = voxel({.face = column.face,
                                      .i = column.i,
                                      .j = column.j,
                                      .layer = static_cast<std::uint32_t>(layer)});
    if (here == nullptr) {
        return std::nullopt;
    }
    return here->solid != globe::Material::Air;
}

void Region::loosen(const globe::Rock& rock) {
    const double reach = std::max({rock.axes.x, rock.axes.y, rock.axes.z});
    const double thickness = world_->store().layers().thickness;
    reshape(rock.middle, reach,
            [&](DVec3 at, DVec3& outward) { return rock.distance(at, outward); },
            [thickness](globe::Voxel& voxel, double to, DVec3 outward) {
                globe::dig_voxel(voxel, to, outward, thickness);
            });
    const double volume = 4.0 / 3.0 * std::numbers::pi * rock.axes.x * rock.axes.y * rock.axes.z;
    parcels_.push_back({.id = ++parcel_ids_,
                        .at = rock.middle,
                        .material = rock.material,
                        .amount = static_cast<std::int64_t>(
                            volume / (thickness * thickness * thickness) * globe::Voxel::full),
                        .radius = rolling_radius(rock)});
}

std::size_t Region::loosen_unsupported(DVec3 point, std::size_t& unjudged) {
    const globe::Layers& layers = world_->store().layers();
    std::size_t freed = 0;
    for (const globe::Rock& rock :
         world_->generator().rocks_near(point, globe::voxel_lod(world_->generator()))) {
        // A rock already taken from its place, or dug away at its middle, is gone.
        const std::optional<bool> there = solid_at(rock.middle);
        if (!there) {
            ++unjudged;
            continue;
        }
        if (!*there) {
            continue;
        }
        // Held up if any of the voxels just under its footprint is ground.
        const DVec3 side = glm::cross(rock.up, rock.across);
        const DVec3 below = rock.middle - (rock.up * (rock.axes.z + (0.75 * layers.thickness)));
        bool unknown = false;
        bool held_up = false;
        const double across[] = {0.0, 0.5, -0.5, 0.0, 0.0};
        const double along[] = {0.0, 0.0, 0.0, 0.5, -0.5};
        for (std::size_t k = 0; k < std::size(across); ++k) {
            const std::optional<bool> ground = solid_at(below + (rock.across * across[k] * rock.axes.x) +
                                                        (side * along[k] * rock.axes.y));
            unknown = unknown || !ground;
            held_up = held_up || ground.value_or(false);
        }
        if (held_up) {
            continue;
        }
        if (unknown) {
            ++unjudged;
            continue;
        }
        loosen(rock);
        ++freed;
    }
    return freed;
}

void Region::loosen_fresh() {
    const globe::Layers& layers = world_->store().layers();
    std::vector<Fresh> waiting;
    for (Fresh& fresh : fresh_) {
        const globe::Site middle{.face = fresh.site.face,
                                 .i = (fresh.site.i * globe::brick_side) + (globe::brick_side / 2),
                                 .j = (fresh.site.j * globe::brick_side) + (globe::brick_side / 2),
                                 .layer = (fresh.site.layer * globe::brick_side) +
                                          (globe::brick_side / 2)};
        std::size_t unjudged = 0;
        loosen_unsupported(globe::middle_of(middle, layers), unjudged);
        if (unjudged > 0 && ++fresh.tries < most_tries) {
            waiting.push_back(fresh);
        }
    }
    fresh_ = std::move(waiting);
}

std::optional<globe::BrickSite> Region::settle_boulder(const Parcel& parcel) {
    const DVec3 up = glm::normalize(parcel.at);
    const DVec3 centre = parcel.at - (up * (settles_in * parcel.radius));
    // Held in full or not at all, so a block is never half built.
    const globe::Layers& layers = world_->store().layers();
    for (const auto& [brick, indices] :
         globe::sites_near(layers, centre, parcel.radius + globe::distance_band(layers))) {
        if (held(brick) == nullptr) {
            return brick;
        }
    }
    reshape(centre, parcel.radius,
            [&](DVec3 at, DVec3& outward) {
                const DVec3 out = at - centre;
                outward = glm::length(out) > 0.0 ? glm::normalize(out) : up;
                return glm::length(out) - parcel.radius;
            },
            [&](globe::Voxel& voxel, double to, DVec3 outward) {
                globe::build_voxel(voxel, to, outward, parcel.material, layers.thickness);
            });
    return std::nullopt;
}

} // namespace hog::flow
