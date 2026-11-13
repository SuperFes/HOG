#include "formed.hpp"

#include "courses.hpp"
#include "joints.hpp"
#include "valleys.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <numbers>
#include <span>
#include <thread>

namespace hog::globe {

namespace {

// Walls stand no steeper than rock does, or loose ground where it lies deep.
constexpr double rock_steepest = 1.73; // 60 degrees
constexpr double loose_steepest = 0.7; // 35 degrees
constexpr double loose_deep = 2.0;     // metres of deposits that slope as loose ground
constexpr double creeping_depth = 0.5; // metres of soil that move as it creeps
// Cells a lake may fill out to from its floor, either way.
constexpr int farthest_fill = 4096;
// The most parts a lake's cells split into across, about its edge.
constexpr int most_fine = 16;
// Rip currents cut crescent bars about this many times the surf zone's width apart.
constexpr double rip_spacing = 4.0;
// Metres the bars, levees and hummocks of what was carried stand on flats.
constexpr double bedforms_stand = 1.0;

// The top of what lies `laid` deep on the whole over a bed spread `spread` either side of its
// mean, over that mean: settled into the bed's hollows, standing in bedforms as high as half of
// it, no higher than they stand on flats and a quarter of the bed's spread.
Noise laid_top(const Noise& laid, const Noise& spread, const Noise& bedforms) noexcept {
    const Settled level = settled(laid.value, spread.value);
    const bool thin = laid.value < 2.0 * (bedforms_stand + (0.25 * spread.value));
    const double high = thin ? 0.5 * laid.value : bedforms_stand + (0.25 * spread.value);
    const DVec3 high_gradient = thin ? laid.gradient * 0.5 : spread.gradient * 0.25;
    return {.value = level.level + (high * bedforms.value),
            .gradient = (laid.gradient * level.per_thick) + (spread.gradient * level.per_spread) +
                        (bedforms.gradient * high) + (high_gradient * bedforms.value)};
}

// The cube face a direction falls on, and where on it, -1 to 1 both ways.
struct OnCube {
    std::size_t axis = 0;
    bool negative = false;
    double u = 0.0;
    double v = 0.0;
};

// A river's channel, as wide and deep as what wears it makes it at its mean (Leopold and
// Maddock's hydraulic geometry down rivers), cut out to hold the floods that overtop its banks
// every year or two, `bankfull` times that mean (the same at one place as its flow rises),
// bending a bend every eleven widths (Leopold and Wolman), more the gentler its fall and the
// looser its banks, its floor as wide as it wanders. The water passing it stands `water` deep.
struct Channel {
    double width = 0.0;
    double depth = 0.0;
    double water = 0.0;
    double meander = 0.0;
    double meander_length = 0.0;
    double belt = 0.0;
};

constexpr double bankfull = 4.0;

// From cubic metres a year wearing it and passing it.
Channel channel_of(double wearing, double passing, double fall, bool loose, double reach) noexcept {
    constexpr double seconds_a_year = 31'557'600.0;
    constexpr double gentle = 0.01; // falls under which rivers wander freely
    const double mean = wearing / seconds_a_year;
    const double mean_depth = 0.27 * maths::pow(mean, 0.4);
    Channel channel{.width = 3.0 * std::sqrt(mean) * maths::pow(bankfull, 0.26),
                    .depth = mean_depth * maths::pow(bankfull, 0.4)};
    if (wearing > 0.0) {
        channel.water =
            std::min(channel.depth, mean_depth * maths::pow(std::max(passing, 0.0) / wearing, 0.4));
    }
    channel.meander_length = 11.0 * channel.width;
    channel.meander = std::min(0.3 * channel.meander_length * (loose ? 1.0 : 0.3) *
                                   std::clamp(1.0 - (fall / gentle), 0.0, 1.0),
                               reach / 4.0);
    channel.belt = channel.meander + (channel.width / 2.0);
    return channel;
}

void add_weighted(Wall& sum, const Wall& one, double weight) noexcept {
    sum.height += weight * one.height;
    sum.gradient += weight * one.gradient;
    sum.floor += weight * one.floor;
    sum.water += weight * one.water;
    sum.bank += weight * one.bank;
    sum.fall += weight * one.fall;
    sum.reaching += weight * one.reaching;
    sum.steep += weight * one.steep;
    sum.standing += weight * one.standing;
    sum.standing_gradient += weight * one.standing_gradient;
    sum.reaching_gradient += weight * one.reaching_gradient;
    sum.cut += weight * one.cut;
    sum.cut_gradient += weight * one.cut_gradient;
}

void divide(Wall& sum, double total) noexcept {
    sum.height /= total;
    sum.gradient /= total;
    sum.floor /= total;
    sum.water /= total;
    sum.bank /= total;
    sum.fall /= total;
    sum.reaching /= total;
    sum.steep /= total;
    sum.standing /= total;
    sum.standing_gradient /= total;
    sum.reaching_gradient /= total;
    sum.cut /= total;
    sum.cut_gradient /= total;
}

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

// A way the ground could stand at a place, the lowest of which it does, and how it falls.
struct GroundWay {
    double height = 0.0;
    DVec3 fall{0.0};
    double counts = 1.0;
};

// The water in a valley's channel at a place, faded as its ground is.
struct ValleyRiver {
    double ground = 0.0;
    double water = 0.0;
    double bank = 0.0;
};

// How high a valley's ground stands over its floor at a place, and where it stands.
struct ValleyFloor {
    double ground = 0.0;
    double above = 0.0;
    DVec3 gradient{0.0};
};

// Metres of a river's water over the ground standing at `top`, the year round and in flood; and
// the level its water stands at either way, metres over the sea, the deepest river's.
struct RiverDepths {
    double river = 0.0;
    double flood = 0.0;
    double level = 0.0;
    double flood_level = 0.0;
};

// A river's water stands only in the channel of the valley the ground lies in, fading out over
// `spread` in height as another's takes over, so none stands over one it doesn't run in.
RiverDepths river_depths(const std::vector<ValleyRiver>& rivers, double top,
                         double spread) noexcept {
    RiverDepths depths;
    for (const ValleyRiver& river : rivers) {
        const double over = std::max(0.0, river.ground - top) / spread;
        if (over >= 1.0) {
            continue;
        }
        // As deep as it stands in its own channel, wherever the ground's merged lower.
        const double in = 1.0 - (over * over * (3.0 - (2.0 * over)));
        if (const double deep = in * (river.water - river.ground); deep > depths.river) {
            depths.river = deep;
            depths.level = river.water;
            depths.flood_level = std::max(river.bank, river.water);
        }
        depths.flood = std::max(depths.flood, in * (river.bank - river.ground));
    }
    return depths;
}

// How far over the floor of the valley it lies in the ground standing at `top` stands, per metre
// over the ground as well.
struct FloorHold {
    double above = 1e30;
    DVec3 gradient{0.0};
};

// Gullies cut down only as far as the floor of the valley the ground lies in, each valley's hold
// fading out over `spread` in height as another's takes over, as its water does.
FloorHold floor_hold(const std::vector<ValleyFloor>& floors, double top, double spread) noexcept {
    FloorHold hold;
    for (const ValleyFloor& floor : floors) {
        const double over = std::max(0.0, floor.ground - top) / spread;
        if (over >= 1.0) {
            continue;
        }
        const double in = 1.0 - (over * over * (3.0 - (2.0 * over)));
        if (const double above = floor.above / in; above < hold.above) {
            hold.above = above;
            hold.gradient = floor.gradient / in;
        }
    }
    return hold;
}

// Water runs off the lowest of the ground's ways, blended with those within `blend` as low by how
// near, so where the ground turns sharply, over crests and where walls meet, the way it runs
// turns smoothly.
DVec3 blended_fall(const std::vector<GroundWay>& ways, double top, double blend) noexcept {
    double blended = 0.0;
    DVec3 fall{0.0};
    for (const GroundWay& way : ways) {
        // A wall rising out of the way may stand under the ground as it is here, fading out.
        const double apart = std::max(0.0, (way.height - top) / blend);
        if (apart < 1.0) {
            const double weight = way.counts * (1.0 - apart) * (1.0 - apart) * (1.0 - apart);
            blended += weight;
            fall += way.fall * weight;
        }
    }
    fall /= blended;
    return fall;
}

// `run` as a share of `length`, or `none` along none.
double share_of(double run, double length, double none) noexcept {
    return length > 0.0 ? run / length : none;
}

// Which way a course runs at each end of its stretch `k`, turning at each bend halfway between
// the stretches either side.
struct Bends {
    DVec3 from{0.0};
    DVec3 to{0.0};
};

Bends bends_at(const std::vector<DVec3>& line, std::size_t k) noexcept {
    const auto way = [&](std::size_t at) { return glm::normalize(line[at + 1] - line[at]); };
    const DVec3 here_way = way(k);
    return {.from = k > 0 ? glm::normalize(way(k - 1) + here_way) : here_way,
            .to = k + 2 < line.size() ? glm::normalize(here_way + way(k + 1)) : here_way};
}

// Which way along the sphere `direction` lies from `on`, none from right over it.
DVec3 outward_from(DVec3 direction, DVec3 on) noexcept {
    const DVec3 off = direction - (glm::normalize(on) * glm::dot(direction, glm::normalize(on)));
    return glm::length(off) > 0.0 ? glm::normalize(off) : DVec3{0.0};
}

} // namespace

Formed::Formed(const Planet& planet)
    : world_{formation_body(planet), 5, static_cast<int>(planet.formed_detail)},
      smooth_{world_.sphere, 2.0}, radius_{planet.body.radius}, voxel_{planet.voxel},
      // The widest gullies half the points' spacing apart, the finest down to a few voxels.
      gullies_{.longest = world_.sphere.spacing() * planet.body.radius / 2.0,
               .octaves = 1 + static_cast<int>(std::floor(
                                  std::log2(world_.sphere.spacing() * planet.body.radius / 2.0 /
                                            (3.0 * planet.voxel)))),
               .seed = hash_values(planet.body.seed, 0x6775'6c6c'6965'73ULL)},
      // As far apart as the widest gullies cut deep, so they fade over their own width at least.
      fall_blend_{gullies_.longest * rock_steepest / 2.0},
      joints_seed_{hash_values(planet.body.seed, 0x6a6f'696e'7473ULL)},
      rock_seed_{hash_values(planet.body.seed, 0x726f'636bULL)},
      faults_seed_{hash_values(planet.body.seed, 0x6661'756c'7473ULL)},
      bars_seed_{hash_values(planet.body.seed, 0x6261'7273ULL)} {
    const formation::World& world = world_;
    const std::uint32_t size = world.sphere.size();
    for (auto* field : {&top_, &organic_, &topsoil_, &subsoil_, &loess_, &sand_, &alluvium_,
                        &lake_beds_, &till_, &saprolite_, &ice_, &lava_, &rock_cover_, &light_,
                        &soil_holds_, &weathering_, &worn_, &years_, &strength_}) {
        field->assign(size, 0.0);
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        const bool land = world.rivers.receiver[point] != point;
        has_sea_ = has_sea_ || !land;
        // The wind took off what rivers and ice laid, and laid sand and loess over it all.
        const double taken = std::min(world.drifts.deflated[point],
                                      world.sediment.alluvium[point] + world.ice.till[point]);
        const double blown = formation::layered(world.drifts.loess[point], world.body) +
                             formation::layered(world.drifts.sand[point], world.body);
        const double ground =
            land ? world.rivers.ground[point] + world.sediment.alluvium[point] - taken + blown
                 : world.rivers.height[point] + world.sediment.delta[point];
        top_[point] = ground + (land ? world.soil.organic[point] : 0.0);
        organic_[point] = world.soil.organic[point];
        topsoil_[point] = world.soil.topsoil[point];
        subsoil_[point] = std::max(0.0, world.soil.depth[point] - world.soil.topsoil[point]);
        loess_[point] = world.soil.loess[point];
        sand_[point] = world.soil.sand[point];
        alluvium_[point] = world.soil.alluvium[point];
        lake_beds_[point] = world.soil.lake_beds[point];
        till_[point] = world.soil.till[point];
        saprolite_[point] = world.soil.saprolite[point];
        ice_[point] = land ? world.ice.thickness[point] : 0.0;
        lava_[point] = world.outcrop.lava[point];
        rock_cover_[point] = world.outcrop.cover[point];
        light_[point] = world.crust.light[point];
        // Under the sea it has had no years to.
        weathering_[point] = formation::soil_making(world.climate.temperature[point],
                                                    world.climate.rain[point], world.body);
        worn_[point] = world.rivers.wear[point];
        years_[point] = world.soil.years[point];
        strength_[point] = world.landforms.strength[point];
        // Soaked through, soil slides on gentler slopes than dry.
        const Properties& soil = properties(Material::Soil);
        const double soaked = formation::waterlogged(formation::wetness(
            world.climate.temperature[point], world.climate.rain[point], world.body));
        soil_holds_[point] =
            maths::tan(soil.repose + ((soil.saturated_repose - soil.repose) * soaked));
    }
    fill_walls();
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
    fill_courses();
    // Lakes below the sea fill on what the sea laid there, under its waves.
    fill_shores();
    fill_lakes();
}

void Formed::fill_walls() {
    const formation::World& world = world_;
    const std::uint32_t size = world.sphere.size();
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
        // As steep as what covers them holds: loose ground its repose, soil as steep as it lies,
        // rock as it stands where bare.
        const double deposits =
            alluvium_[point] + lake_beds_[point] + till_[point] + sand_[point] + loess_[point];
        const double soil = organic_[point] + topsoil_[point] + subsoil_[point];
        double holds = soil > creeping_depth ? soil_holds_[point] : rock_steepest;
        if (deposits > loose_deep) {
            holds = loose_steepest;
        }
        wall_[point] = std::min(steepest, holds);
    }
}

DVec3 Formed::Lake::toward(double x, double y) const noexcept {
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
            lake_cap_[point] = world.lakes.lakes[lake].level;
        }
    }

    lakes_.resize(floor.size());
    std::atomic<std::size_t> next = 0;
    const auto fill = [&] {
        formation::Smooth::Weights weights;
        for (std::size_t lake = next++; lake < floor.size(); lake = next++) {
            if (floor[lake] != none) {
                lakes_[lake] =
                    fill_lake(floor[lake], reach[lake], world.lakes.lakes[lake].level, weights);
            }
        }
    };
    {
        std::vector<std::thread> workers;
        const unsigned count = std::max(1U, std::thread::hardware_concurrency());
        workers.reserve(count);
        for (unsigned worker = 0; worker < count; ++worker) {
            workers.emplace_back(fill);
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    lakes_near_.assign(size, {});
    std::vector<std::uint32_t> listed(size, none);
    lake_levels_.assign(lakes_.size(), std::nullopt);
    for (std::uint32_t lake = 0; lake < lakes_.size(); ++lake) {
        if (floor[lake] != none) {
            list_lake(lake, floor[lake], listed);
            if (!lakes_[lake].pool.cells.empty()) {
                lake_levels_[lake] = lakes_[lake].pool.level;
            }
        }
    }
}

namespace {

// Points with the sea on one side and land on the other.
std::vector<std::uint8_t> at_waterline(const formation::World& world) {
    std::vector<std::uint8_t> held(world.sphere.size(), 0);
    for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
        const bool sea = world.rivers.receiver[point] == point;
        held[point] = std::ranges::any_of(world.sphere.neighbors(point),
                                          [&](std::uint32_t near) {
                                              return (world.rivers.receiver[near] == near) != sea;
                                          })
                          ? 1
                          : 0;
    }
    return held;
}

// Points next to `held` ones take their mean in each field, and are held from then on.
void spread_round(const formation::Sphere& sphere, std::span<std::vector<double>* const> fields,
                  std::vector<std::uint8_t>& held) {
    std::vector<std::uint8_t> next = held;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (held[point] != 0) {
            continue;
        }
        int count = 0;
        for (const std::uint32_t near : sphere.neighbors(point)) {
            if (held[near] != 0) {
                ++count;
                for (std::vector<double>* field : fields) {
                    (*field)[point] += (*field)[near];
                }
            }
        }
        if (count > 0) {
            for (std::vector<double>* field : fields) {
                (*field)[point] /= count;
            }
            next[point] = 1;
        }
    }
    held = std::move(next);
}

// Points about where `top` comes out of the water, and a point either side of them.
std::vector<std::uint8_t> about_waterline(const formation::Sphere& sphere,
                                          const std::vector<double>& top) {
    std::vector<std::uint8_t> about(sphere.size(), 0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const bool crossing = std::ranges::any_of(sphere.neighbors(point), [&](std::uint32_t near) {
            return (top[near] > 0.0) != (top[point] > 0.0);
        });
        if (crossing) {
            about[point] = 1;
            for (const std::uint32_t near : sphere.neighbors(point)) {
                about[near] = 1;
            }
        }
    }
    return about;
}

// Points with none in `at` take a neighbour's.
void share_round(const formation::Sphere& sphere, std::vector<std::uint32_t>& at) {
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> shared = at;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (at[point] != none) {
            continue;
        }
        const auto neighbors = sphere.neighbors(point);
        const auto near =
            std::ranges::find_if(neighbors, [&](std::uint32_t next) { return at[next] != none; });
        if (near != neighbors.end()) {
            shared[point] = at[*near];
        }
    }
    at = std::move(shared);
}

// The sand there's most of.
Material most_sand(double quartz, double volcanic, double carbonate) {
    if (quartz >= volcanic && quartz >= carbonate) {
        return Material::Sand;
    }
    return volcanic >= carbonate ? Material::VolcanicSand : Material::ShellSand;
}

} // namespace

void Formed::fill_shores() {
    const formation::World& world = world_;
    const formation::Shores& shores = world.shores;
    const double gravity = world.body.gravity;
    for (const auto& [at, material] :
         {std::pair{0, Material::Sand}, std::pair{1, Material::VolcanicSand},
          std::pair{2, Material::ShellSand}, std::pair{3, Material::Shingle},
          std::pair{4, Material::Sediment}}) {
        grains_[static_cast<std::size_t>(at)] = grains_of(material, gravity);
    }
    waves_ = shores.waves;
    usual_ = shores.usual;
    beach_ = shores.beach;
    mud_ = shores.mud;
    quartz_ = shores.quartz;
    volcanic_ = shores.volcanic;
    carbonate_ = shores.carbonate;
    shingle_ = shores.shingle;
    const std::array fields{&waves_,  &usual_,    &beach_,     &mud_,
                            &quartz_, &volcanic_, &carbonate_, &shingle_};
    std::vector<std::uint8_t> held = at_waterline(world);
    // The ground between the points sinks towards a deep sea beside them, so it comes out of the
    // water up to a few points inland of where they have the coast.
    constexpr int rounds = 6;
    for (int round = 0; round < rounds; ++round) {
        spread_round(world.sphere, fields, held);
    }
    lay_shores();
}

void Formed::lay_shores() {
    const formation::Sphere& sphere = world_.sphere;
    const std::uint32_t size = sphere.size();
    const std::vector<std::uint8_t> building = about_waterline(sphere, top_);
    std::vector<std::optional<Laid>> built(size);
    std::atomic<std::uint32_t> next = 0;
    const auto build = [&] {
        formation::Smooth::Weights weights;
        for (std::uint32_t point = next++; point < size; point = next++) {
            if (building[point] != 0) {
                built[point] = lay_shore(point, weights);
            }
        }
    };
    {
        std::vector<std::thread> workers;
        const unsigned count = std::max(1U, std::thread::hardware_concurrency());
        workers.reserve(count);
        for (unsigned worker = 0; worker < count; ++worker) {
            workers.emplace_back(build);
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    }
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    laid_at_.assign(size, none);
    laid_.clear();
    for (std::uint32_t point = 0; point < size; ++point) {
        if (const std::optional<Laid>& here = built[point]; here.has_value()) {
            laid_at_[point] = static_cast<std::uint32_t>(laid_.size());
            laid_.push_back(here.value());
        }
    }
    // Reads weigh points a couple of spacings about, so those beside share a neighbour's.
    for (int round = 0; round < 2; ++round) {
        share_round(sphere, laid_at_);
    }
}

std::optional<Laid> Formed::lay_shore(std::uint32_t point,
                                      formation::Smooth::Weights& weights) const {
    const std::optional<Beach> beach =
        beach_from([&](const std::vector<double>& field) { return field[point]; });
    if (!beach) {
        return std::nullopt;
    }
    const formation::Sphere& sphere = world_.sphere;
    const DVec3 middle = sphere.point(point);
    smooth_.weigh(middle, weights, point);
    const formation::Smooth::Read here = formation::Smooth::read(top_, weights);
    const DVec3 rise = here.gradient - (glm::dot(here.gradient, middle) * middle);
    if (glm::length(rise) <= 0.0) {
        return std::nullopt;
    }
    // The ground across the shore, down its fall from a couple of spacings inland to as far out.
    const DVec3 seaward = -glm::normalize(rise);
    const double spacing = sphere.spacing() * radius_;
    constexpr int steps = static_cast<int>(Section::samples) - 1;
    std::array<double, steps + 1> along{};
    std::array<double, steps + 1> height{};
    std::uint32_t near = point;
    for (int at = 0; at <= steps; ++at) {
        const auto index = static_cast<std::size_t>(at);
        along[index] = spacing * ((4.0 * at / steps) - 2.0);
        smooth_.weigh(glm::normalize(middle + (seaward * (along[index] / radius_))), weights, near);
        near = weights.nearest;
        height[index] = formation::Smooth::read(top_, weights).value;
    }
    // Where it comes out of the water nearest the point.
    std::optional<double> water;
    std::size_t from = 0;
    for (std::size_t at = 0; at < steps; ++at) {
        if (height[at] > 0.0 && height[at + 1] <= 0.0) {
            const double there = along[at] + ((along[at + 1] - along[at]) * height[at] /
                                              (height[at] - height[at + 1]));
            if (!water || std::abs(there) < std::abs(*water)) {
                water = there;
                from = at;
            }
        }
    }
    if (!water) {
        return std::nullopt;
    }
    // Out from the waterline, never rising seaward: out from it the lowest yet, in from it the
    // highest. Where it's level its height can't tell how far out it is, so it's read as falling
    // at least as the beach's own shoreface does on the whole.
    Section section;
    for (std::size_t at = 0; at <= steps; ++at) {
        section.out[at] = along[at] - *water;
        section.height[at] = height[at];
    }
    const Profile& profile = beach->profile;
    const double least_fall =
        (along[1] - along[0]) * profile.closure / std::max(out_to(profile, profile.closure), 1.0);
    for (std::size_t at = from + 1; at <= steps; ++at) {
        section.height[at] = std::min(section.height[at], section.height[at - 1] - least_fall);
    }
    for (std::size_t at = from + 1; at-- > 0;) {
        section.height[at] = std::max(section.height[at], section.height[at + 1] + least_fall);
    }
    const std::optional<Built> laid = built_along(profile, section, beach->volume);
    if (!laid) {
        return std::nullopt;
    }
    return Laid{.profile = profile,
                .built = *laid,
                .bars = bars_on(profile, beach->sea, beach->grains, world_.body.gravity),
                .section = section};
}

template <typename Value>
std::optional<Formed::Beach> Formed::beach_from(const Value& value) const {
    const double storm = value(waves_);
    const double sand = std::max(0.0, value(beach_));
    const double mud = std::max(0.0, value(mud_));
    if (storm <= 0.0 || (sand <= 0.0 && mud <= 0.0)) {
        return std::nullopt;
    }
    const formation::Body& body = world_.body;
    Beach beach;
    beach.sea = {.storm = storm,
                 .usual = std::clamp(value(usual_), 0.0, storm),
                 .period = formation::wave_period(storm, body)};
    // Where more mud than sand comes, it's mud flats.
    beach.mud = mud > sand;
    if (beach.mud) {
        beach.grains = grains_[4];
        beach.volume = mud;
    } else {
        std::array shares{std::max(0.0, value(quartz_)), std::max(0.0, value(volcanic_)),
                          std::max(0.0, value(carbonate_)), std::max(0.0, value(shingle_))};
        double all = 0.0;
        for (const double share : shares) {
            all += share;
        }
        if (all <= 0.0) {
            shares = {1.0, 0.0, 0.0, 0.0};
            all = 1.0;
        }
        for (double& share : shares) {
            share /= all;
        }
        beach.grains = mixed(std::span{grains_}.first<4>(), shares);
        beach.volume = sand;
    }
    beach.profile = profile_for(beach.sea, beach.grains, body.gravity);
    // The swash throws up sand and stones, not mud: mud flats have no berm.
    if (beach.mud) {
        beach.profile.berm = 0.0;
    }
    return beach;
}

void Formed::lay_beach(DVec3 direction, const formation::Smooth::Weights& weights,
                       const Surface& under, Noise& top, Strata& layers) const {
    // As thick over the ground as the points' beaches lie over ground as high, blended.
    const formation::Smooth::Read plane = formation::Smooth::read(top_, weights);
    const DVec3 rise =
        (plane.gradient - (glm::dot(plane.gradient, direction) * direction)) / radius_;
    double thick = 0.0;
    double rate = 0.0;
    DVec3 weighed{0.0};
    for (std::size_t at = 0; at < weights.points.size(); ++at) {
        const std::uint32_t index = laid_at_[weights.points[at]];
        if (index == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const Laid& laid = laid_[index];
        double shift = 0.0;
        if (laid.bars.count > 0) {
            const double spacing =
                rip_spacing * laid.bars.crest[static_cast<std::size_t>(laid.bars.count - 1)];
            shift = laid.bars.rhythm *
                    gradient_noise(direction * (radius_ / spacing), bars_seed_).value;
        }
        const Across lying = lying_over(laid, plane.value, shift);
        const double weight = weights.weights[at];
        thick += weight * lying.height;
        rate += weight * lying.slope;
        weighed += weights.gradients[at] * lying.height;
    }
    if (thick <= 0.0) {
        return;
    }
    const DVec3 thickening = (weighed / radius_) + (rate * rise);
    // The waves spread it along the shore they build rather than into the hollows under it: it
    // fills them up to its top, standing in bars and hummocks over it, as deep again as it lies
    // thick and no deeper, there being no more of it; over deeper ones it lies that thick,
    // following them down. Ground standing over its top it leaves bare.
    const Noise settled_top =
        laid_top({.value = thick, .gradient = thickening}, Noise{}, under.bedforms);
    const double over = plane.value + settled_top.value - top.value;
    if (over <= 0.0) {
        return;
    }
    const bool draping = over >= 2.0 * thick;
    const double fill = draping ? 2.0 * thick : over;
    top.gradient = draping ? top.gradient + (2.0 * thickening) : rise + settled_top.gradient;
    top.value += fill;
    layers.beach = fill;
    layers.organic = 0.0;
    layers.topsoil = 0.0;
    layers.subsoil = 0.0;
    // The swash throws stones to the top of the beach, sand lies under them.
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const std::optional<Beach> beach = beach_from(value);
    if (!beach) {
        layers.beach_of = Material::Sand;
        return;
    }
    const double shingle = std::clamp(value(shingle_), 0.0, 1.0);
    const Profile& profile = beach->profile;
    if (beach->mud) {
        layers.beach_of = Material::Sediment;
    } else if (top.value >= profile.berm - (shingle * (profile.berm + profile.closure))) {
        layers.beach_of = Material::Shingle;
    } else {
        layers.beach_of = most_sand(value(quartz_), value(volcanic_), value(carbonate_));
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
    // Cells a sixteenth of the points' spacing, finer than the ground read between them bends.
    // Its own reach runs a few spacings past its points; rising, it fills as far out as the
    // ground holds it.
    water.step = sphere.spacing() / 16.0;
    const int half = std::clamp(
        static_cast<int>(std::ceil((reach + (3.0 * sphere.spacing())) / water.step)), 16, 2048);
    std::uint32_t near = floor;
    const auto ground = [&](double x, double y) {
        const DVec3 direction = water.toward(x, y);
        smooth_.weigh(direction, weights, near);
        near = weights.nearest;
        return surface(direction, weights, 0.0).ground.top.value;
    };
    // A cell's middle may stand over the water while ground in it lies under: as much as its
    // own width up, on all but cliffs, gullies finer than it among them. About its edge, cells
    // split as fine as the finest gullies, so it finds its way through rims as narrow.
    const double cell = water.step * radius_;
    const int fine = std::clamp(static_cast<int>(std::ceil(cell / (3.0 * voxel_))), 1, most_fine);
    water.pool = fill_pool(half, ground, most, cell, farthest_fill, fine);
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

std::vector<Formed::Outlet> Formed::lake_outlets() const {
    std::vector<Outlet> outlets;
    for (const Lake& water : lakes_) {
        if (water.pool.outlet) {
            outlets.push_back(
                {.at = water.toward(water.pool.outlet->first, water.pool.outlet->second),
                 .level = water.pool.level});
        }
    }
    return outlets;
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

void Formed::fill_courses() {
    courses_.assign(world_.sphere.size(), {});
    std::atomic<std::uint32_t> next{0};
    const auto fill = [&] {
        for (std::uint32_t point = next++; point < courses_.size(); point = next++) {
            if (world_.rivers.receiver[point] != point) {
                courses_[point] = make_course(point);
            }
        }
    };
    std::vector<std::thread> workers;
    const unsigned count = std::max(1U, std::thread::hardware_concurrency());
    workers.reserve(count);
    for (unsigned worker = 0; worker < count; ++worker) {
        workers.emplace_back(fill);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

std::vector<DVec3> Formed::make_course(std::uint32_t point) const {
    const formation::World& world = world_;
    const formation::Sphere& sphere = world.sphere;
    const std::uint32_t down = world.rivers.receiver[point];
    const DVec3 from = sphere.point(point);
    const DVec3 to = sphere.point(down);
    const bool mouth = world.rivers.receiver[down] == down;
    const double floor_to = mouth ? std::min(top_[point], 0.0) : top_[down];
    // The rock about it varies from place to place, finer than its points can tell, at every
    // scale down to what its lanes tell apart; faults crush it.
    const Octaves rocks{
        .wavelength = sphere.spacing() * radius_ / 2.0, .gain = 0.55, .finest = 20.0};
    formation::Smooth::Weights weights;
    std::uint32_t near = point;
    const auto effort = [&](DVec3 at) {
        smooth_.weigh(at, weights, near);
        near = weights.nearest;
        const auto value = [&](const std::vector<double>& field) {
            return formation::Smooth::read(field, weights).value;
        };
        // As much as it has to cut down to its floor where it runs, and its own depth, which
        // it cuts wherever it runs.
        constexpr double channel_depth = 1.0;
        const DVec3 along = to - from;
        const double t = std::clamp(glm::dot(at - from, along) / glm::dot(along, along), 0.0, 1.0);
        const double floor = top_[point] + ((floor_to - top_[point]) * t);
        const double cut = std::max(0.0, value(top_) - floor) + channel_depth;
        // Loose ground cuts as the weakest rock does, but for how it lies.
        const double deposits =
            value(alluvium_) + value(lake_beds_) + value(till_) + value(sand_) + value(loess_);
        const DVec3 metres = at * radius_;
        const double strength = std::clamp((deposits > loose_deep ? 0.0 : value(strength_)) +
                                               (0.5 * fbm(metres, rocks, 0.0, rock_seed_).value) -
                                               faulted(metres, faults_seed_),
                                           0.0, 1.0);
        // As slowly as the rock wears (the rivers' own rule for rock strength).
        return cut * maths::exp(std::numbers::ln10 * world.body.rock_hardness * (strength - 0.5));
    };
    const double apart = maths::atan2(glm::length(glm::cross(from, to)), glm::dot(from, to));
    return course({.from = from, .to = to, .half_width = 0.3 * apart, .steps = 16, .lanes = 5},
                  effort);
}

Formed::Carved Formed::carved(DVec3 direction, const formation::Smooth::Weights& weights,
                              double lod) const {
    const formation::World& world = world_;
    const formation::Sphere& sphere = world.sphere;
    const formation::Smooth::Read smooth = formation::Smooth::read(top_, weights);
    Carved carved{.top = smooth, .fall = smooth.gradient, .above_floor = 1e30};
    // The ways the ground could stand here, the lowest of which it does, and how each falls.
    thread_local std::vector<GroundWay> ways;
    ways.clear();
    // The water in each valley's channel, faded as its ground is.
    thread_local std::vector<ValleyRiver> rivers;
    rivers.clear();
    // How high each valley's ground stands over its floor, and where it stands.
    thread_local std::vector<ValleyFloor> floors;
    floors.clear();
    ways.push_back({.height = smooth.value, .fall = smooth.gradient, .counts = 1.0});
    // Where walls meet, at crests, soil creeping off rounds them over as far as creep carries
    // it against the ground wearing down (a hilltop's steady parabola reaches the walls' slope
    // D·S/E out), no further than a hillside runs. Bare rock stays as sharp as its joints let it:
    // a crest narrower than a block topples a block at a time, so none is thinner than the joints
    // are apart. Merged as smoothly in height as that width climbs, as far as both walls are
    // there: the ground as its points have it is no wall, nor is a valley faded into it.
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const double soil = value(organic_) + value(topsoil_) + value(subsoil_);
    const double creep = world.body.soil_creep * std::min(1.0, soil / creeping_depth);
    const double walls = value(wall_);
    const double rounding =
        std::max(std::min(creep * walls / std::max(value(worn_), world.body.slowest_wear),
                          sphere.spacing() * radius_ / 4.0),
                 joint_spacing(value(strength_)) / 2.0);
    const double blend = 2.0 * walls * rounding;
    // The ground as the lowest of the valleys has it, its points' own among them.
    Lowered lowered{rounding / radius_, blend};
    lowered.add(smooth, 0.0, smooth.value);
    // Walls reach a little way from their valleys, so none is ever missed among those of the
    // points weighed here, however the points weighed change.
    const double reach = 0.6 * sphere.spacing() * radius_;
    // Valleys wander off the lines their courses are drawn on at every scale down to the finest
    // asked, so their floors, walls and the reach of each ragged out of true; never so sharply
    // that the ground folds over, so their walls stand a little steeper or gentler and their
    // floors wider or narrower as they go.
    const DVec3 metres = direction * radius_;
    const Octaves wander{
        .wavelength = 2.0 * sphere.spacing() * radius_, .gain = 0.35, .finest = 3.0 * voxel_};
    constexpr double sharpest_wander = 0.9;
    const double wandering = sharpest_wander / (std::numbers::sqrt3 * fbm_steepest(wander));
    const std::array<Noise, 3> wanders{fbm(metres, wander, lod, hash_values(rock_seed_, 7)),
                                       fbm(metres, wander, lod, hash_values(rock_seed_, 8)),
                                       fbm(metres, wander, lod, hash_values(rock_seed_, 9))};
    const DVec3 wandered = glm::normalize(
        metres + (DVec3{wanders[0].value, wanders[1].value, wanders[2].value} * wandering));
    // A slope over the wandered ground back to the ground's own.
    const auto unwandered = [&](DVec3 slope) {
        const DVec3 back =
            slope + (((wanders[0].gradient * slope.x) + (wanders[1].gradient * slope.y) +
                      (wanders[2].gradient * slope.z)) *
                     wandering);
        return back - (direction * glm::dot(back, direction));
    };
    // A valley is read here as far as its points are weighed here, so none comes or goes at
    // once as the points weighed change.
    const auto valley = [&](std::uint32_t point, double weighed) {
        constexpr double weighed_fully = 0.02;
        const double near = std::min(1.0, weighed / weighed_fully);
        const double present = near * near * (3.0 - (2.0 * near));
        if (present <= 0.0) {
            return;
        }
        std::optional<Wall> read = valley_wall(point, wandered, reach, lod, rounding, blend);
        if (!read) {
            return;
        }
        Wall& wall = *read;
        for (DVec3* slope : {&wall.gradient, &wall.fall, &wall.standing_gradient,
                             &wall.reaching_gradient, &wall.cut_gradient}) {
            *slope = unwandered(*slope);
        }
        // Out of its reach a valley fades into the ground as its points have it. Read's
        // gradient is per radian.
        const double reaching = wall.reaching * present;
        const auto fade = [&](double height) {
            return (reaching * height) + ((1.0 - reaching) * smooth.value);
        };
        // Its water too, fading out as its floor rises over the ground about it.
        rivers.push_back(
            {.ground = fade(wall.standing), .water = fade(wall.water), .bank = fade(wall.bank)});
        if (wall.standing < smooth.value) {
            const double faded = fade(wall.standing);
            const DVec3 fading =
                ((wall.standing_gradient * radius_) * reaching) +
                (smooth.gradient * (1.0 - reaching)) +
                ((wall.reaching_gradient * radius_) * present * (wall.standing - smooth.value));
            lowered.add({.value = faded, .gradient = fading}, reaching, fade(wall.floor));
        }
        if (reaching > 0.0) {
            ways.push_back(
                {.height = wall.standing, .fall = wall.fall * radius_, .counts = reaching});
        }
        // Its floor holds the gullies back as far as it reaches.
        if (reaching > 0.0) {
            floors.push_back({.ground = fade(wall.standing),
                              .above = (wall.standing - wall.floor) / reaching,
                              .gradient = wall.standing_gradient / reaching});
        }
    };
    for (std::size_t at = 0; at < weights.points.size(); ++at) {
        const std::uint32_t point = weights.points[at];
        valley(point, weights.weights[at]);
        // And those running into it from points not weighed, as far as it is.
        for (const std::uint32_t up : sphere.neighbors(point)) {
            if (world.rivers.receiver[up] == point &&
                std::ranges::find(weights.points, up) == weights.points.end()) {
                valley(up, weights.weights[at]);
            }
        }
    }
    carved.top = lowered.lowest();
    const double spread = std::max(blend, voxel_);
    const RiverDepths depths = river_depths(rivers, carved.top.value, spread);
    carved.river = depths.river;
    carved.flood = depths.flood;
    carved.river_level = depths.level;
    carved.flood_level = depths.flood_level;
    const FloorHold hold = floor_hold(floors, carved.top.value, spread);
    carved.above_floor = hold.above;
    carved.above_floor_gradient = hold.gradient;
    carved.fall = blended_fall(ways, carved.top.value, fall_blend_);
    return carved;
}

std::optional<Wall> Formed::valley_wall(std::uint32_t point, DVec3 direction, double reach,
                                        double lod, double rounding, double blend) const {
    const formation::World& world = world_;
    const formation::Sphere& sphere = world.sphere;
    const std::uint32_t down = world.rivers.receiver[point];
    if (down == point) {
        return std::nullopt;
    }
    // A river's bed meets the sea at its level, not down on the sea floor past its mouth.
    const bool mouth = world.rivers.receiver[down] == down;
    const double floor_to = mouth ? std::min(top_[point], 0.0) : top_[down];
    // Out of reach of all its course, it's faded into the ground here.
    const DVec3 from = sphere.point(point);
    const DVec3 to = sphere.point(down);
    const double chord = glm::length(to - from);
    const double bound = (0.5 * chord) + (0.3 * chord) + (reach / radius_);
    if (glm::length(direction - (0.5 * (from + to))) > bound) {
        return std::nullopt;
    }
    // Its floor falling evenly along its course.
    const std::vector<DVec3>& line = course_of(point);
    thread_local std::vector<double> runs;
    runs.assign(line.size(), 0.0);
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        runs[k + 1] = runs[k] + glm::length(line[k + 1] - line[k]);
    }
    const double length = runs.back();
    const double metres = length * radius_;
    const double fall = metres > 0.0 ? (top_[point] - floor_to) / metres : 0.0;
    const auto channel_at = [&](std::uint32_t at, double falling) {
        Channel made = channel_of(world.rivers.flood[at], world.lakes.discharge[at], falling,
                                  alluvium_[at] + lake_beds_[at] + till_[at] > loose_deep, reach);
        // Channels too narrow for the detail asked fade out with their water, as gullies do.
        const double shown = std::clamp((made.width / (2.0 * lod)) - 1.0, 0.0, 1.0);
        made.depth *= shown;
        made.water *= shown;
        return made;
    };
    const Channel channel = channel_at(point, fall);
    // Its channel and water meet the next one's as they are where it runs into it; at the sea, it
    // brims to the sea's level.
    Channel next = channel;
    if (mouth) {
        next.water = channel.depth;
    } else {
        next = channel_at(down, fall);
    }
    // Running into a lake, its water stands no lower than the lake's, and comes down to it where
    // it runs in: as deep in its channel there as the lake stands over its floor, brimming at most.
    double least_water = -std::numeric_limits<double>::infinity();
    if (const std::uint32_t lake = world.lakes.lake[down]; lake != formation::Lakes::none &&
                                                           lake < lake_levels_.size() &&
                                                           lake_levels_[lake].has_value()) {
        least_water = lake_levels_[lake].value();
        next.water = std::clamp(least_water - floor_to + next.depth, 0.0, next.depth);
    }
    // Its walls stand less steep than their rock lets them in places, as each has been worn,
    // so where they meet another valley's the crest between runs ragged.
    const Noise worn =
        fbm(direction * radius_,
            {.wavelength = sphere.spacing() * radius_, .gain = 0.5, .finest = 8.0 * voxel_}, lod,
            hash_values(rock_seed_, 10, point));
    constexpr double least_steep = 0.35;
    const double steep_share = std::clamp(0.65 + (0.45 * worn.value), least_steep, 1.0);
    const double steepest_here = std::max(wall_[point], wall_[down]);
    const double slope = steepest_here * steep_share;
    const DVec3 slope_gradient = steep_share > least_steep && steep_share < 1.0
                                     ? worn.gradient * (0.45 * steepest_here)
                                     : DVec3{0.0};
    const auto floor_at = [&](double run) {
        return top_[point] + ((floor_to - top_[point]) * share_of(run, length, 0.0));
    };
    // The ground stands as low as the lowest stretch of its course has it, where a bend
    // brings two near, blended over a few metres between them so it never breaks.
    constexpr double blending = 5.0;
    thread_local std::vector<double> rough;
    thread_local std::vector<double> least;
    rough.assign(line.size() - 1, 0.0);
    least.assign(line.size() - 1, 0.0);
    double lowest = 1e300;
    // Each stretch stands no lower than its floor and walls with the channel cut out of them, and
    // no higher than the walls' foot rounded over the belt; any that might stand lowest is read,
    // so none comes lowest with a step.
    const double deepest = std::max(channel.depth, next.depth);
    double surely = 1e300;
    // How far the whole course lies, and which way from its nearest.
    double nearest = 1e300;
    DVec3 outward{0.0};
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        const DVec3 along = line[k + 1] - line[k];
        const double t = std::clamp(glm::dot(direction - line[k], along) /
                                        std::max(glm::dot(along, along), 1e-300),
                                    0.0, 1.0);
        const DVec3 on = line[k] + (along * t);
        const double away = glm::length(direction - on) * radius_;
        const double walls = floor_at(runs[k] + (t * (runs[k + 1] - runs[k]))) +
                             (slope * std::max(0.0, away - channel.belt));
        rough[k] = walls + rise_past(away, reach);
        lowest = std::min(lowest, rough[k]);
        least[k] = walls - deepest;
        surely = std::min(surely, walls + (slope * channel.belt / 4.0));
        if (away < nearest) {
            nearest = away;
            outward = outward_from(direction, on);
        }
    }
    // The line's chord and arc differ by far less than this, so none is missed by their
    // difference.
    constexpr double slack = 0.01;
    Wall wall{.height = 0.0, .reaching = 0.0};
    double stretches = 0.0;
    Lowest lowest_of{rounding, blend};
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        // Weighed down to none at all by the furthest blended, so none drops out with a step.
        constexpr double furthest = 6.0;
        const double weight = maths::exp(-(rough[k] - lowest) / blending) - maths::exp(-furthest);
        if (weight <= 0.0 && least[k] > surely + std::max(handing_over, blend) + slack) {
            continue;
        }
        const Bends bends = bends_at(line, k);
        const Wall one = wall_at({.from = line[k],
                                  .to = line[k + 1],
                                  .floor_from = floor_at(runs[k]),
                                  .floor_to = floor_at(runs[k + 1]),
                                  .wall = slope,
                                  .wall_gradient = slope_gradient,
                                  .reach = reach,
                                  .head = share_of(runs[k], length, 0.0),
                                  .foot = share_of(runs[k + 1], length, 1.0),
                                  .length = metres,
                                  .belt = channel.belt,
                                  .channel_width = channel.width,
                                  .channel_depth = channel.depth,
                                  .channel_depth_to = next.depth,
                                  .water_depth = channel.water,
                                  .water_depth_to = next.water,
                                  .least_water = least_water,
                                  .meander = channel.meander,
                                  .meander_length = std::max(channel.meander_length, 1.0),
                                  .seed = hash_values(rock_seed_, point),
                                  .runs_from = bends.from,
                                  .runs_to = bends.to},
                                 direction, radius_);
        if (weight > 0.0) {
            stretches += weight;
            add_weighted(wall, one, weight);
        }
        lowest_of.add(one);
    }
    divide(wall, stretches);
    lowest_of.lower(wall);
    // The whole valley fades into the ground about it out of its reach of the course as a whole,
    // not of each stretch, past half of it.
    const double half = reach / 2.0;
    const double past = std::clamp((nearest - half) / half, 0.0, 1.0);
    wall.reaching = 1.0 - (past * past * (3.0 - (2.0 * past)));
    wall.reaching_gradient = outward * (-6.0 * past * (1.0 - past) / half);
    return wall;
}

Formed::Surface Formed::surface(DVec3 direction, const formation::Smooth::Weights& weights,
                                double lod) const {
    const Carved ground = carved(direction, weights, lod);
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };

    const double deposits =
        value(alluvium_) + value(lake_beds_) + value(till_) + value(sand_) + value(loess_);
    const double loose = std::clamp(deposits / loose_deep, 0.0, 1.0);
    // A lake is what the ground about it drains to: gullies run down to its level, and under it
    // its beds fill them. Its pull fades out with the share of its points about.
    double above_floor = ground.above_floor;
    DVec3 above_floor_gradient = ground.above_floor_gradient;
    if (const auto [share, share_gradient] = formation::Smooth::read(lake_share_, weights);
        share > 0.0) {
        constexpr double any = 0.05;
        const double near = std::min(1.0, share / any);
        const DVec3 nearing = share < any ? share_gradient / any : DVec3{0.0};
        const double out_of_reach = gullies_.longest * rock_steepest;
        const auto [cap, cap_gradient] = formation::Smooth::read(lake_cap_, weights);
        const double level = cap / share;
        const double lake_above = ground.top.value - level +
                                  ((1.0 - (near * near * (3.0 - (2.0 * near)))) * out_of_reach);
        if (lake_above < above_floor) {
            above_floor = lake_above;
            const DVec3 levelling = (cap_gradient - (share_gradient * level)) / share;
            above_floor_gradient = (ground.top.gradient - levelling -
                                    (nearing * (6.0 * near * (1.0 - near) * out_of_reach))) /
                                   radius_;
        }
    }
    // Between the valleys its points can tell, the ground is as rough as the hollows and spurs
    // too small for them leave it: as rough at every scale for its size, twisted as the rock it's
    // cut from varies, standing as high as the walls would climb across a twentieth of each. Soil
    // creeping smooths away what's narrower than it can carry soil across in the years it has had
    // (creep is diffusion, so 2π√(Dt)); bare rock stays rough. Valley floors stay smooth: it fades
    // in over its own height above them.
    const double longest = sphere_spacing_metres() / 2.0;
    const double soil = value(organic_) + value(topsoil_) + value(subsoil_);
    const double creep = world_.body.soil_creep * std::min(1.0, soil / creeping_depth);
    const double smoothed = 2.0 * std::numbers::pi * std::sqrt(creep * value(years_));
    const Octaves rough{
        .wavelength = longest, .gain = 0.5, .finest = std::clamp(smoothed, 3.0 * voxel_, longest)};
    const DVec3 metres = direction * radius_;
    const Octaves twist{.wavelength = longest, .gain = 0.5, .finest = longest / 16.0};
    const double twisting = 0.3 * longest;
    const std::array<Noise, 3> twists{fbm(metres, twist, lod, hash_values(rock_seed_, 1)),
                                      fbm(metres, twist, lod, hash_values(rock_seed_, 2)),
                                      fbm(metres, twist, lod, hash_values(rock_seed_, 3))};
    const DVec3 twisted =
        metres + (DVec3{twists[0].value, twists[1].value, twists[2].value} * twisting);
    // The bumps wider than the gullies, which turn and deepen them as the ground under them does,
    // and those narrower, which the gullies cut through.
    const Noise wide =
        fbm(twisted, rough, lod, hash_values(rock_seed_, 4), {.shorter_than = gullies_.longest});
    const Noise narrow =
        fbm(twisted, rough, lod, hash_values(rock_seed_, 4), {.longest = gullies_.longest});
    // Through the twist back to the ground's own metres.
    const auto untwisted = [&](DVec3 gradient) {
        return gradient + ((twists[0].gradient * gradient.x) + (twists[1].gradient * gradient.y) +
                           (twists[2].gradient * gradient.z)) *
                              twisting;
    };
    const auto [walls, walls_gradient] = formation::Smooth::read(wall_, weights);
    const double height = 0.05 * walls * longest;
    // As high as it stands, less near the floor.
    const double above = std::max(0.0, above_floor);
    const double standing = std::min(height, above);
    DVec3 standing_gradient{0.0};
    if (above < height) {
        standing_gradient = above > 0.0 ? above_floor_gradient : DVec3{0.0};
    } else {
        standing_gradient = walls_gradient * (0.05 * longest / radius_);
    }
    const auto tilt = [&](const Noise& bumps) {
        const DVec3 rising =
            (untwisted(bumps.gradient) * standing) + (standing_gradient * bumps.value);
        return rising - (glm::dot(rising, direction) * direction);
    };
    const double raised_wide = wide.value * standing;
    const double raised = raised_wide + (narrow.value * standing);
    const DVec3 tilt_wide = tilt(wide);
    const DVec3 tilt_narrow = tilt(narrow);
    Dissected dissected = dissect(
        direction, radius_,
        {.top = {.value = ground.top.value + raised,
                 .gradient = (ground.top.gradient / radius_) + tilt_wide + tilt_narrow},
         .fall = (ground.fall / radius_) + tilt_wide,
         .broad_fall = formation::Smooth::read(top_, weights).gradient / radius_,
         .above_floor = std::max(0.0, above_floor + raised_wide),
         .above_floor_gradient =
             above_floor + raised_wide > 0.0 ? above_floor_gradient + tilt_wide : DVec3{0.0},
         .steepest = rock_steepest + ((loose_steepest - rock_steepest) * loose),
         .ice = value(ice_)},
        gullies_, lod);
    // Its hollows were worn out of the bed as the gullies were; its spurs stand over it.
    dissected.cut -= raised;
    dissected.cut_gradient -= tilt_wide + tilt_narrow;
    dissected.spread.value += standing;
    dissected.spread.gradient += standing_gradient;
    Surface cut{.ground = dissected,
                .river = ground.river,
                .flood = ground.flood,
                .river_level = ground.river_level,
                .flood_level = ground.flood_level,
                .bed = dissected.top.value,
                .settled = 1.0,
                .bedforms = Noise{}};
    // Ice fills the gullies itself, what it carried lying under it.
    if (dissected.ice <= 0.0) {
        settle(direction, weights, lod, cut);
    }
    return cut;
}

void Formed::settle(DVec3 direction, const formation::Smooth::Weights& weights, double lod,
                    Surface& surface) const {
    const Octaves forms{.wavelength = gullies_.longest / 4.0, .gain = 0.5, .finest = 3.0 * voxel_};
    const Noise bedforms = fbm(direction * radius_, forms, lod, hash_values(rock_seed_, 5));
    surface.bedforms = {.value = bedforms.value,
                        .gradient = bedforms.gradient -
                                    (glm::dot(bedforms.gradient, direction) * direction)};
    Noise laid;
    for (const std::vector<double>* field : {&sand_, &alluvium_, &lake_beds_, &till_}) {
        const formation::Smooth::Read read = formation::Smooth::read(*field, weights);
        laid.value += read.value;
        laid.gradient += read.gradient;
    }
    if (laid.value <= 0.0) {
        return;
    }
    laid.gradient = (laid.gradient - (glm::dot(laid.gradient, direction) * direction)) / radius_;
    Dissected& ground = surface.ground;
    // Some of it drapes the bed as it lies, the rest settles into its hollows.
    const Noise share = draping(direction, weights, lod, ground);
    const Noise drape{.value = laid.value * share.value,
                      .gradient = (laid.gradient * share.value) + (share.gradient * laid.value)};
    const Noise settling{.value = laid.value - drape.value,
                         .gradient = laid.gradient - drape.gradient};
    const Noise top = laid_top(settling, ground.spread, surface.bedforms);
    // The bed stands its cut under its uncut height; it was laid as deep as it stood thick over
    // that, so all of what was laid is taken off the top, and what lies here put back.
    const double over = top.value + ground.cut;
    surface.settled = (std::max(0.0, over) + drape.value) / laid.value;
    ground.top.value += std::max(0.0, over) + drape.value - laid.value;
    ground.top.gradient += drape.gradient - laid.gradient;
    if (over > 0.0) {
        ground.top.gradient += top.gradient + ground.cut_gradient;
    }
}

Noise Formed::draping(DVec3 direction, const formation::Smooth::Weights& weights, double lod,
                      const Dissected& ground) const {
    // Under the sea what's carried hangs in the water, and below where storms' waves stir the bed
    // (half their length down) it settles where it falls: the deeper under that, the more of it,
    // as patchily as the currents carry it.
    const double deep = -(ground.top.value + ground.cut);
    if (deep <= 0.0) {
        return {};
    }
    const double storm = std::max(0.0, formation::Smooth::read(waves_, weights).value);
    const double period = formation::wave_period(storm, world_.body);
    const double base = world_.body.gravity * period * period / (4.0 * std::numbers::pi);
    const double share = deep / (deep + base);
    const DVec3 sharing =
        (ground.top.gradient + ground.cut_gradient) * (-base / ((deep + base) * (deep + base)));
    const Octaves currents{
        .wavelength = gullies_.longest, .gain = 0.5, .finest = gullies_.longest / 8.0};
    const Noise patches = fbm(direction * radius_, currents, lod, hash_values(rock_seed_, 6));
    const double patchy = share * (1.0 + (0.5 * patches.value));
    if (patchy <= 0.0) {
        return {};
    }
    if (patchy >= 1.0) {
        return {.value = 1.0, .gradient = DVec3{0.0}};
    }
    const DVec3 patching = patches.gradient - (glm::dot(patches.gradient, direction) * direction);
    return {.value = patchy,
            .gradient = (sharing * (1.0 + (0.5 * patches.value))) + (patching * (0.5 * share))};
}

Formed::Here Formed::at(DVec3 direction, double lod) const {
    formation::Smooth::Weights weights;
    smooth_.weigh(direction, weights, start_near(direction));
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const Surface under = surface(direction, weights, lod);
    Dissected top = under.ground;

    Here here;
    const formation::World& world = world_;
    // A lake's water stands level over what of its cells lies lower.
    for (const std::uint32_t lake : lakes_near_[weights.nearest]) {
        const Lake& water = lakes_[lake];
        const double toward = glm::dot(direction, water.middle);
        if (toward <= 0.0) {
            continue;
        }
        const auto cell = [&](const DVec3& axis) {
            return glm::dot(direction, axis) / toward / water.step;
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
    layers.sand = value(sand_) * under.settled;
    layers.alluvium = value(alluvium_) * under.settled;
    layers.lake_beds = value(lake_beds_) * under.settled;
    layers.till = value(till_) * under.settled;
    layers.saprolite = value(saprolite_);
    layers.lava = value(lava_);
    // Gullies cut down through what lies draped over the ground and into the rock under it, the
    // top of which is what's rotted; under ice, which nothing settled under, through all of it.
    here.cut = top.cut;
    double cut = std::max(0.0, top.cut);
    const std::array all{&layers.loess,     &layers.sand, &layers.alluvium,
                         &layers.lake_beds, &layers.till, &layers.lava};
    const std::array draped{&layers.loess, &layers.lava};
    for (double* laid :
         top.ice > 0.0 ? std::span<double* const>{all} : std::span<double* const>{draped}) {
        const double taken = std::min(*laid, cut);
        *laid -= taken;
        cut -= taken;
    }
    layers.saprolite = std::max(0.0, layers.saprolite - cut);
    // Under what covers it the rock rots in from its joints as it's brought up to the top (Linton's
    // tors): the narrowest blocks rot right through, the widest leave rounded cores, which stand
    // out of the rotted rock as it's carried off. What lies over the rock buries the cores
    // standing lower than it's deep; one standing higher comes out of it as a crag, bare. On
    // ground steeper than their rubble rests on, blocks slide and topple out along the joints as
    // fast as the ground under them wears, weathered or not, the rock's jointing setting how it
    // stands (Schmidt and Montgomery).
    const double strength = value(strength_);
    const double spacing = joint_spacing(strength);
    const DVec3 cut_rise = top.top.gradient - (glm::dot(top.top.gradient, direction) * direction);
    const double cut_slope = glm::length(cut_rise);
    const double holds = cut_slope / value(soil_holds_);
    const double over = ((layers.organic + layers.topsoil + layers.subsoil) *
                         std::max(0.0, 1.0 - (holds * holds))) +
                        layers.loess + layers.sand + layers.alluvium + layers.lake_beds +
                        layers.till + layers.lava + layers.saprolite;
    here.weathering = {.rate = value(weathering_),
                       .worn = value(worn_),
                       .years = value(years_),
                       .strength = strength};
    const double steep = std::min(1.0, cut_slope / maths::tan(properties(Material::Gravel).repose));
    here.weathering.loosening =
        top.ice > 0.0 ? 0.0
                      : std::min(here.weathering.worn,
                                 here.weathering.rate + (here.weathering.worn * steep * steep));
    // Ice scours off what rotted.
    const double rotted = top.ice > 0.0
                              ? 0.0
                              : rotted_by(here.weathering.rate,
                                          std::max(here.weathering.worn, world_.body.slowest_wear),
                                          world_.body.soil_falloff, here.weathering.years);
    // Rock shows where nothing covers it, or where the cores of blocks twice as wide as the joints
    // are apart stand out of what does.
    if (over <= 0.0) {
        here.weathering.bare = 1.0;
    } else if (const double tallest = std::min(rotted, 2.0 * (spacing - rotted)); tallest > 0.0) {
        here.weathering.bare = std::max(0.0, 1.0 - (over / tallest));
    }
    // Metres the rock's top rises with its core. Where a river runs, up to its floods, the water
    // plucks the blocks loose along their joints and carries them off faster than cores rot out:
    // none stands in its channel.
    double rock_rises = 0.0;
    const bool flowing = under.river > 0.0 && top.top.value - over < under.flood_level;
    if (rotted > 0.0 && !flowing) {
        const DVec3 ground = direction * (radius_ + top.top.value - over);
        const Core core = core_over(block_at(ground - (direction * 1e-3), strength, joints_seed_),
                                    rotted, ground, direction);
        // Cores too narrow for the detail asked fade out, as gullies do.
        const double fade = std::clamp((core.breadth / (2.0 * lod)) - 1.0, 0.0, 1.0);
        if (const double relief = core.height * fade; relief > over) {
            here.crag = relief - over;
            rock_rises = relief;
            top.top.value += here.crag;
            top.top.gradient += core.rise * fade;
            for (double* loose : {&layers.organic, &layers.topsoil, &layers.subsoil, &layers.loess,
                                  &layers.sand, &layers.alluvium, &layers.lake_beds, &layers.till,
                                  &layers.lava, &layers.saprolite}) {
                *loose = 0.0;
            }
        }
    }
    const double rock_cover = std::max(0.0, value(rock_cover_) - cut + rock_rises);
    lay_beach(direction, weights, under, top.top, layers);
    here.top = top.top;
    // A river stands at its own level along its course, one level across its channel, wherever
    // the ground's lower: bars in it stand dry.
    if (const double bed = here.top.value - here.crag;
        under.river > 0.0 && !here.lake && under.river_level > bed) {
        here.river = Here::River{.level = under.river_level, .flood = under.flood_level};
    }
    // Soil forms again on whatever's at the top, but creeps off the faster the nearer the slope
    // comes to the steepest it holds (Roering's critical slope), none lying past it.
    const DVec3 rise = top.top.gradient - (glm::dot(top.top.gradient, direction) * direction);
    const double steepness = glm::length(rise) / value(soil_holds_);
    const double lying = std::max(0.0, 1.0 - (steepness * steepness));
    layers.organic *= lying;
    layers.topsoil *= lying;
    layers.subsoil *= lying;
    // Rotted rock is the top of the rock itself, so the beds under it lie no deeper for it.
    layers.cover = layers.ice + layers.beach + layers.organic + layers.topsoil + layers.subsoil +
                   layers.loess + layers.sand + layers.alluvium + layers.lake_beds + layers.till +
                   layers.lava + std::max(rock_cover, layers.saprolite);
    layers.basement = here.light >= 0.5 ? Material::Granite : Material::Basalt;
    return here;
}

} // namespace hog::globe
