#include "formed.hpp"

#include "valleys.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <thread>

namespace hog::globe {

namespace {

// Walls stand no steeper than rock does, or loose ground where it lies deep.
constexpr double rock_steepest = 1.73; // 60 degrees
constexpr double loose_steepest = 0.7; // 35 degrees
constexpr double loose_deep = 2.0;     // metres of deposits that slope as loose ground

formation::Body body_for(const Planet& planet) {
    formation::Body body;
    body.seed = planet.seed;
    body.tilt = planet.tilt;
    return body;
}

// The cube face a direction falls on, and where on it, -1 to 1 both ways.
struct OnCube {
    std::size_t axis = 0;
    bool negative = false;
    double u = 0.0;
    double v = 0.0;
};

OnCube on_cube(DVec3 direction) noexcept {
    const DVec3 size = glm::abs(direction);
    std::size_t axis = 0;
    if (size.y > size.x && size.y >= size.z) {
        axis = 1;
    } else if (size.z > size.x && size.z > size.y) {
        axis = 2;
    }
    const double major = std::max(size[axis], 1e-300);
    return {.axis = axis,
            .negative = direction[axis] < 0.0,
            .u = direction[(axis + 1) % 3] / major,
            .v = direction[(axis + 2) % 3] / major};
}

} // namespace

Formed::Formed(const Planet& planet, const Derived& derived)
    : world_{body_for(planet), 5, static_cast<int>(planet.formed_detail)},
      smooth_{world_.sphere, 2.0}, radius_{planet.radius}, vertical_{derived.vertical},
      // The widest gullies half the points' spacing apart, the finest down to a few voxels.
      gullies_{.longest = world_.sphere.spacing() * planet.radius / 2.0,
               .octaves =
                   1 + static_cast<int>(std::floor(std::log2(
                           world_.sphere.spacing() * planet.radius / 2.0 / (3.0 * planet.voxel)))),
               .seed = hash_values(planet.seed, 0x6775'6c6c'6965'73ULL)},
      // As far apart as the widest gullies cut deep, so they fade over their own width at least.
      fall_blend_{gullies_.longest * rock_steepest / 2.0} {
    const formation::World& world = world_;
    const std::uint32_t size = world.sphere.size();
    for (auto* field : {&top_, &organic_, &topsoil_, &subsoil_, &loess_, &sand_, &alluvium_,
                        &lake_beds_, &till_, &saprolite_, &ice_, &lava_, &rock_cover_, &light_}) {
        field->assign(size, 0.0);
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        const bool land = world.rivers.receiver[point] != point;
        has_sea_ = has_sea_ || !land;
        // The ice, rock and deposits stand as tall against the planet as its relief does; the
        // soil and what lies on it keep their true depths.
        // The wind took off what rivers and ice laid, and laid sand and loess over it all.
        const double taken = std::min(world.drifts.deflated[point],
                                      world.sediment.alluvium[point] + world.ice.till[point]);
        const double blown = formation::layered(world.drifts.loess[point], world.body) +
                             formation::layered(world.drifts.sand[point], world.body);
        const double ground =
            land ? world.rivers.ground[point] + world.sediment.alluvium[point] - taken + blown
                 : world.rivers.height[point] + world.sediment.delta[point];
        top_[point] = (vertical_ * ground) + (land ? world.soil.organic[point] : 0.0);
        organic_[point] = world.soil.organic[point];
        topsoil_[point] = world.soil.topsoil[point];
        subsoil_[point] = std::max(0.0, world.soil.depth[point] - world.soil.topsoil[point]);
        loess_[point] = vertical_ * world.soil.loess[point];
        sand_[point] = vertical_ * world.soil.sand[point];
        alluvium_[point] = vertical_ * world.soil.alluvium[point];
        lake_beds_[point] = vertical_ * world.soil.lake_beds[point];
        till_[point] = vertical_ * world.soil.till[point];
        saprolite_[point] = vertical_ * world.soil.saprolite[point];
        ice_[point] = land ? vertical_ * world.ice.thickness[point] : 0.0;
        lava_[point] = vertical_ * world.outcrop.lava[point];
        rock_cover_[point] = vertical_ * world.outcrop.cover[point];
        light_[point] = world.crust.light[point];
    }
    // Walls as steep as the ground falls to the point's nearest, no steeper than they stand.
    wall_.assign(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        double steepest = 0.0;
        for (const std::uint32_t next : world.sphere.neighbors(point)) {
            const double run =
                std::acos(std::clamp(glm::dot(world.sphere.point(point), world.sphere.point(next)),
                                     -1.0, 1.0)) *
                radius_;
            steepest = std::max(steepest, std::abs(top_[next] - top_[point]) / std::max(run, 1.0));
        }
        const double deposits =
            alluvium_[point] + lake_beds_[point] + till_[point] + sand_[point] + loess_[point];
        wall_[point] = std::min(steepest, deposits > loose_deep ? loose_steepest : rock_steepest);
    }
    const auto [lowest, highest] = std::ranges::minmax(top_);
    lowest_ = lowest;
    highest_ = highest;

    const auto across = static_cast<double>(starts_across);
    starts_.resize(6 * starts_across * starts_across);
    std::uint32_t near = 0;
    for (std::size_t face = 0; face < 6; ++face) {
        const std::size_t axis = face / 2;
        for (std::size_t i = 0; i < starts_across; ++i) {
            for (std::size_t j = 0; j < starts_across; ++j) {
                DVec3 direction{0.0};
                direction[axis] = face % 2 == 0 ? 1.0 : -1.0;
                direction[(axis + 1) % 3] = ((2.0 * (static_cast<double>(i) + 0.5)) / across) - 1.0;
                direction[(axis + 2) % 3] = ((2.0 * (static_cast<double>(j) + 0.5)) / across) - 1.0;
                near = world.sphere.nearest(glm::normalize(direction), near);
                starts_[(((face * starts_across) + i) * starts_across) + j] = near;
            }
        }
    }
    fill_lakes();
}

DVec3 Formed::Lake::toward(int x, int y) const noexcept {
    return glm::normalize(middle + (east * (x * step)) + (north * (y * step)));
}

void Formed::fill_lakes() {
    const formation::World& world = world_;
    const formation::Sphere& sphere = world.sphere;
    const std::uint32_t size = sphere.size();
    constexpr std::uint32_t none = formation::Lakes::none;
    // Each lake's floor, the lowest of its points, and how far its points reach from there.
    std::vector<std::uint32_t> floor(world.lakes.lakes.size(), none);
    std::vector<double> reach(floor.size(), 0.0);
    const auto lake_at = [&](std::uint32_t point) {
        const bool land = world.rivers.receiver[point] != point;
        return land && world.sediment.lake_depth[point] > 0.0 ? world.lakes.lake[point] : none;
    };
    for (std::uint32_t point = 0; point < size; ++point) {
        if (const std::uint32_t lake = lake_at(point);
            lake != none && (floor[lake] == none || top_[point] < top_[floor[lake]])) {
            floor[lake] = point;
        }
    }
    lake_share_.assign(size, 0.0);
    lake_cap_.assign(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (const std::uint32_t lake = lake_at(point); lake != none) {
            const double dot = glm::dot(sphere.point(point), sphere.point(floor[lake]));
            reach[lake] = std::max(reach[lake], std::acos(std::clamp(dot, -1.0, 1.0)));
            lake_share_[point] = 1.0;
            lake_cap_[point] = vertical_ * world.lakes.lakes[lake].level;
        }
    }

    lakes_.resize(floor.size());
    std::atomic<std::size_t> next = 0;
    const auto fill = [&] {
        formation::Smooth::Weights weights;
        for (std::size_t lake = next++; lake < floor.size(); lake = next++) {
            if (floor[lake] != none) {
                lakes_[lake] = fill_lake(floor[lake], reach[lake],
                                         vertical_ * world.lakes.lakes[lake].level, weights);
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        const unsigned count = std::max(1U, std::thread::hardware_concurrency());
        workers.reserve(count);
        for (unsigned worker = 0; worker < count; ++worker) {
            workers.emplace_back(fill);
        }
    }

    lakes_near_.assign(size, {});
    std::vector<std::uint32_t> listed(size, none);
    for (std::uint32_t lake = 0; lake < lakes_.size(); ++lake) {
        if (floor[lake] != none) {
            list_lake(lake, floor[lake], listed);
        }
    }
}

Formed::Lake Formed::fill_lake(std::uint32_t floor, double reach, double most,
                               formation::Smooth::Weights& weights) const {
    const formation::Sphere& sphere = world_.sphere;
    Lake water;
    water.middle = sphere.point(floor);
    const DVec3 pole = std::abs(water.middle.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    water.east = glm::normalize(glm::cross(pole, water.middle));
    water.north = glm::cross(water.middle, water.east);
    // Cells a sixteenth of the points' spacing, finer than the ground read between them bends,
    // with room to brim past its points by a few spacings.
    water.step = sphere.spacing() / 16.0;
    const int half = std::clamp(
        static_cast<int>(std::ceil((reach + (3.0 * sphere.spacing())) / water.step)), 16, 2048);
    std::uint32_t near = floor;
    const auto ground = [&](int x, int y) {
        const DVec3 direction = water.toward(x, y);
        smooth_.weigh(direction, weights, near);
        near = weights.nearest;
        return surface(direction, weights, 0.0).top.value;
    };
    // A cell's middle may stand over the water while ground in it lies under: as much as its
    // own width up, on all but cliffs, gullies finer than it among them.
    water.pool = fill_pool(half, ground, most, water.step * radius_);
    return water;
}

void Formed::list_lake(std::uint32_t lake, std::uint32_t floor,
                       std::vector<std::uint32_t>& listed) {
    // At the points nearest its cells and beside them, for reads to find it by.
    const formation::Sphere& sphere = world_.sphere;
    const Lake& water = lakes_[lake];
    const auto list = [&](std::uint32_t point) {
        if (listed[point] != lake) {
            listed[point] = lake;
            lakes_near_[point].push_back(lake);
        }
    };
    std::uint32_t near = floor;
    for (int y = water.pool.y0; y < water.pool.y0 + water.pool.height; ++y) {
        for (int x = water.pool.x0; x < water.pool.x0 + water.pool.width; ++x) {
            if (water.pool.at(x, y) == Pool::Cell::Dry) {
                continue;
            }
            near = sphere.nearest(water.toward(x, y), near);
            list(near);
            for (const std::uint32_t beside : sphere.neighbors(near)) {
                list(beside);
            }
        }
    }
}

std::uint32_t Formed::start_near(DVec3 direction) const noexcept {
    const OnCube on = on_cube(direction);
    const auto across = static_cast<double>(starts_across);
    const auto cell = [&](double along) {
        return static_cast<std::size_t>(
            std::clamp((along + 1.0) * 0.5 * across, 0.0, across - 1.0));
    };
    const std::size_t face = (on.axis * 2) + (on.negative ? 1 : 0);
    return starts_[(((face * starts_across) + cell(on.u)) * starts_across) + cell(on.v)];
}

Formed::Carved Formed::carved(DVec3 direction, const formation::Smooth::Weights& weights) const {
    const formation::World& world = world_;
    const formation::Sphere& sphere = world.sphere;
    const formation::Smooth::Read smooth = formation::Smooth::read(top_, weights);
    Carved carved{.top = smooth, .fall = smooth.gradient, .above_floor = 1e30};
    // The ways the ground could stand here, the lowest of which it does, and how each falls.
    struct Way {
        double height = 0.0;
        DVec3 fall{0.0};
        double counts = 1.0;
    };
    thread_local std::vector<Way> ways;
    ways.clear();
    ways.push_back({.height = smooth.value, .fall = smooth.gradient, .counts = 1.0});
    // Walls reach a little way from their valleys, so none is ever missed among those of the
    // points weighed here, however the points weighed change.
    const double reach = 0.6 * sphere.spacing() * radius_;
    const auto valley = [&](std::uint32_t point) {
        const std::uint32_t down = world.rivers.receiver[point];
        if (down == point) {
            return;
        }
        // A river's bed meets the sea at its level, not down on the sea floor past its mouth.
        const bool mouth = world.rivers.receiver[down] == down;
        const Wall wall = wall_at({.from = sphere.point(point),
                                   .to = sphere.point(down),
                                   .floor_from = top_[point],
                                   .floor_to = mouth ? std::min(top_[point], 0.0) : top_[down],
                                   .wall = std::max(wall_[point], wall_[down]),
                                   .reach = reach},
                                  direction, radius_);
        // Read's gradient is per radian.
        if (wall.height < carved.top.value) {
            carved.top = {.value = wall.height, .gradient = wall.gradient * radius_};
        }
        if (wall.reaching > 0.0) {
            ways.push_back(
                {.height = wall.standing, .fall = wall.fall * radius_, .counts = wall.reaching});
        }
        carved.above_floor = std::min(carved.above_floor, wall.height - wall.floor);
    };
    for (const std::uint32_t point : weights.points) {
        valley(point);
        // And those running into it from points not weighed.
        for (const std::uint32_t up : sphere.neighbors(point)) {
            if (world.rivers.receiver[up] == point &&
                std::ranges::find(weights.points, up) == weights.points.end()) {
                valley(up);
            }
        }
    }
    // Water runs off the lowest, blended with those near as low by how near, so where the ground
    // turns sharply, over crests and where walls meet, the way it runs turns smoothly.
    double blended = 0.0;
    carved.fall = DVec3{0.0};
    for (const Way& way : ways) {
        // A wall rising out of the way may stand under the ground as it is here, fading out.
        const double apart = std::max(0.0, (way.height - carved.top.value) / fall_blend_);
        if (apart < 1.0) {
            const double weight = way.counts * (1.0 - apart) * (1.0 - apart) * (1.0 - apart);
            blended += weight;
            carved.fall += way.fall * weight;
        }
    }
    carved.fall /= blended;
    return carved;
}

Dissected Formed::surface(DVec3 direction, const formation::Smooth::Weights& weights,
                          double lod) const {
    const Carved ground = carved(direction, weights);
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };

    const double deposits =
        value(alluvium_) + value(lake_beds_) + value(till_) + value(sand_) + value(loess_);
    const double loose = std::clamp(deposits / loose_deep, 0.0, 1.0);
    // A lake is what the ground about it drains to: gullies run down to its level, and under it
    // its beds fill them. Its pull fades out with the share of its points about.
    double above_floor = ground.above_floor;
    if (const double share = value(lake_share_); share > 0.0) {
        constexpr double any = 0.05;
        const double near = std::min(1.0, share / any);
        const double out_of_reach = gullies_.longest * rock_steepest;
        above_floor = std::min(above_floor,
                               ground.top.value - (value(lake_cap_) / share) +
                                   ((1.0 - (near * near * (3.0 - (2.0 * near)))) * out_of_reach));
    }
    return dissect(direction, radius_,
                   {.top = {.value = ground.top.value, .gradient = ground.top.gradient / radius_},
                    .fall = ground.fall / radius_,
                    .above_floor = std::max(0.0, above_floor),
                    .steepest = rock_steepest + ((loose_steepest - rock_steepest) * loose),
                    .ice = value(ice_)},
                   gullies_, lod);
}

Formed::Here Formed::at(DVec3 direction, double lod) const {
    formation::Smooth::Weights weights;
    smooth_.weigh(direction, weights, start_near(direction));
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const Dissected top = surface(direction, weights, lod);

    Here here;
    here.top = top.top;
    const formation::World& world = world_;
    // A lake's water stands level over what of its cells lies lower.
    for (const std::uint32_t lake : lakes_near_[weights.nearest]) {
        const Lake& water = lakes_[lake];
        const double toward = glm::dot(direction, water.middle);
        if (toward <= 0.0) {
            continue;
        }
        const auto cell = [&](const DVec3& axis) {
            return static_cast<int>(std::lround(glm::dot(direction, axis) / toward / water.step));
        };
        if (water.pool.at(cell(water.east), cell(water.north)) != Pool::Cell::Dry &&
            water.pool.level > top.top.value + 0.01) {
            here.lake = water.pool.level;
            break;
        }
    }
    here.light = value(light_);
    here.folding = value(world.bedrock.folding);
    Strata& layers = here.layers;
    layers.ice = top.ice;
    layers.organic = value(organic_);
    layers.topsoil = value(topsoil_);
    layers.subsoil = value(subsoil_);
    layers.loess = value(loess_);
    layers.sand = value(sand_);
    layers.alluvium = value(alluvium_);
    layers.lake_beds = value(lake_beds_);
    layers.till = value(till_);
    layers.saprolite = value(saprolite_);
    layers.lava = value(lava_);
    // Rotted rock is the top of the rock itself, so the beds under it lie no deeper for it.
    layers.cover = layers.ice + layers.organic + layers.topsoil + layers.subsoil + layers.loess +
                   layers.sand + layers.alluvium + layers.lake_beds + layers.till + layers.lava +
                   std::max(value(rock_cover_), layers.saprolite);
    layers.basement = here.light >= 0.5 ? Material::Granite : Material::Basalt;
    return here;
}

} // namespace hog::globe
