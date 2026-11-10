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

// The cube face a direction falls on, and where on it, -1 to 1 both ways.
struct OnCube {
    std::size_t axis = 0;
    bool negative = false;
    double u = 0.0;
    double v = 0.0;
};

// A river's channel, as wide and deep as its water makes it (Leopold and Maddock's hydraulic
// geometry), bending a bend every eleven widths (Leopold and Wolman), more the gentler its fall
// and the looser its banks, its floor as wide as it wanders.
struct Channel {
    double width = 0.0;
    double depth = 0.0;
    double meander = 0.0;
    double meander_length = 0.0;
    double belt = 0.0;
};

Channel channel_of(double discharge, double fall, bool loose, double reach) noexcept {
    constexpr double seconds_a_year = 31'557'600.0;
    constexpr double gentle = 0.01; // falls under which rivers wander freely
    const double water = discharge / seconds_a_year;
    Channel channel{.width = 3.0 * std::sqrt(water), .depth = 0.27 * maths::pow(water, 0.4)};
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
    sum.fall += weight * one.fall;
    sum.reaching += weight * one.reaching;
    sum.standing += weight * one.standing;
    sum.standing_gradient += weight * one.standing_gradient;
    sum.reaching_gradient += weight * one.reaching_gradient;
}

void divide(Wall& sum, double total) noexcept {
    sum.height /= total;
    sum.gradient /= total;
    sum.floor /= total;
    sum.fall /= total;
    sum.reaching /= total;
    sum.standing /= total;
    sum.standing_gradient /= total;
    sum.reaching_gradient /= total;
}

// The lower of the ground standing so far and another way it could, `wall` of it a wall, merged
// over `blend` in height as far as both are walls: a quadratic smooth minimum, its slope shared
// between the two as they near.
void merge_lower(formation::Smooth::Read& top, double& top_wall, double height, DVec3 gradient,
                 double wall, double blend) noexcept {
    const double width = blend * wall * top_wall;
    const double apart = std::abs(height - top.value);
    const bool lower = height < top.value;
    if (apart >= width) {
        if (lower) {
            top = {.value = height, .gradient = gradient};
            top_wall = wall;
        }
        return;
    }
    const double share = (width - apart) / width;
    const double low = lower ? height : top.value;
    const DVec3 low_gradient = lower ? gradient : top.gradient;
    const DVec3 high_gradient = lower ? top.gradient : gradient;
    const double low_wall = lower ? wall : top_wall;
    const double high_wall = lower ? top_wall : wall;
    top = {.value = low - (share * share * width / 4.0),
           .gradient = (low_gradient * (1.0 - (share / 2.0))) + (high_gradient * (share / 2.0))};
    top_wall = (low_wall * (1.0 - (share / 2.0))) + (high_wall * (share / 2.0));
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
    courses_.resize(size);
    course_made_ = std::vector<std::once_flag>(size);
    fill_lakes();
    fill_shores();
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
    for (std::uint32_t lake = 0; lake < lakes_.size(); ++lake) {
        if (floor[lake] != none) {
            list_lake(lake, floor[lake], listed);
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

void Formed::lay_beach(DVec3 direction, const formation::Smooth::Weights& weights, Noise& top,
                       Strata& layers) const {
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
    // Hollows cut below the points' ground it fills up to its top, as deep again as it lies thick
    // and no deeper, there being no more of it; over deeper ones it lies that thick, following them
    // down. Ground standing over theirs it fills only up to its top, and none stands higher.
    const double over = plane.value + thick - top.value;
    if (over <= 0.0) {
        return;
    }
    const bool draping = over >= 2.0 * thick;
    const double fill = draping ? 2.0 * thick : over;
    top.gradient = (draping ? top.gradient : rise) + ((draping ? 2.0 : 1.0) * thickening);
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
        return surface(direction, weights, 0.0).top.value;
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

const std::vector<DVec3>& Formed::course_of(std::uint32_t point) const {
    std::call_once(course_made_[point], [&] {
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
            const double t =
                std::clamp(glm::dot(at - from, along) / glm::dot(along, along), 0.0, 1.0);
            const double floor = top_[point] + ((floor_to - top_[point]) * t);
            const double cut = std::max(0.0, value(top_) - floor) + channel_depth;
            // Loose ground cuts as the weakest rock does, but for how it lies.
            const double deposits =
                value(alluvium_) + value(lake_beds_) + value(till_) + value(sand_) + value(loess_);
            const DVec3 metres = at * radius_;
            const double strength =
                std::clamp((deposits > loose_deep ? 0.0 : value(strength_)) +
                               (0.5 * fbm(metres, rocks, 0.0, rock_seed_).value) -
                               faulted(metres, faults_seed_),
                           0.0, 1.0);
            // As slowly as the rock wears (the rivers' own rule for rock strength).
            return cut *
                   maths::exp(std::numbers::ln10 * world.body.rock_hardness * (strength - 0.5));
        };
        const double apart = maths::atan2(glm::length(glm::cross(from, to)), glm::dot(from, to));
        courses_[point] = course(
            {.from = from, .to = to, .half_width = 0.3 * apart, .steps = 16, .lanes = 5}, effort);
    });
    return courses_[point];
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
    // Where walls meet, at crests, soil creeping off rounds them over as far as creep carries
    // it against the ground wearing down (a hilltop's steady parabola reaches the walls' slope
    // D·S/E out), no further than a hillside runs; bare rock stays sharp. Merged as smoothly in
    // height as that width climbs, as far as both walls are there: the ground as its points have
    // it is no wall, nor is a valley faded into it.
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    const double soil = value(organic_) + value(topsoil_) + value(subsoil_);
    const double creep = world.body.soil_creep * std::min(1.0, soil / creeping_depth);
    const double walls = value(wall_);
    const double rounding =
        std::min(creep * walls / std::max(value(worn_), world.body.slowest_wear),
                 sphere.spacing() * radius_ / 4.0);
    const double blend = 2.0 * walls * rounding;
    // How much of a wall the ground standing so far is.
    double top_wall = 0.0;
    // Walls reach a little way from their valleys, so none is ever missed among those of the
    // points weighed here, however the points weighed change.
    const double reach = 0.6 * sphere.spacing() * radius_;
    // A valley is read here as far as its points are weighed here, so none comes or goes at
    // once as the points weighed change.
    const auto valley = [&](std::uint32_t point, double weighed) {
        constexpr double weighed_fully = 0.02;
        const double near = std::min(1.0, weighed / weighed_fully);
        const double present = near * near * (3.0 - (2.0 * near));
        if (present <= 0.0) {
            return;
        }
        const std::optional<Wall> read = valley_wall(point, direction, reach);
        if (!read) {
            return;
        }
        const Wall& wall = *read;
        // Out of its reach a valley fades into the ground as its points have it. Read's
        // gradient is per radian.
        const double reaching = wall.reaching * present;
        if (wall.standing < smooth.value) {
            const double faded = (reaching * wall.standing) + ((1.0 - reaching) * smooth.value);
            const DVec3 fading =
                ((wall.standing_gradient * radius_) * reaching) +
                (smooth.gradient * (1.0 - reaching)) +
                ((wall.reaching_gradient * radius_) * present * (wall.standing - smooth.value));
            merge_lower(carved.top, top_wall, faded, fading, reaching, blend);
        }
        if (reaching > 0.0) {
            ways.push_back(
                {.height = wall.standing, .fall = wall.fall * radius_, .counts = reaching});
        }
        if (const double above = (wall.height - wall.floor) / present; above < carved.above_floor) {
            carved.above_floor = above;
            carved.above_floor_gradient = wall.gradient / present;
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

std::optional<Wall> Formed::valley_wall(std::uint32_t point, DVec3 direction, double reach) const {
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
    const Channel channel =
        channel_of(world.rivers.discharge[point], fall,
                   alluvium_[point] + lake_beds_[point] + till_[point] > loose_deep, reach);
    const double slope = std::max(wall_[point], wall_[down]);
    const auto floor_at = [&](double run) {
        return top_[point] + ((floor_to - top_[point]) * (length > 0.0 ? run / length : 0.0));
    };
    // The ground stands as low as the lowest stretch of its course has it, where a bend
    // brings two near, blended over a few metres between them so it never breaks.
    constexpr double blending = 5.0;
    thread_local std::vector<double> rough;
    rough.assign(line.size() - 1, 0.0);
    double lowest = 1e300;
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        const DVec3 along = line[k + 1] - line[k];
        const double t = std::clamp(glm::dot(direction - line[k], along) /
                                        std::max(glm::dot(along, along), 1e-300),
                                    0.0, 1.0);
        const double away = glm::length(direction - (line[k] + (along * t))) * radius_;
        rough[k] = floor_at(runs[k] + (t * (runs[k + 1] - runs[k]))) +
                   (slope * std::max(0.0, away - channel.belt)) + rise_past(away, reach);
        lowest = std::min(lowest, rough[k]);
    }
    const auto way = [&](std::size_t k) { return glm::normalize(line[k + 1] - line[k]); };
    Wall wall{.height = 0.0, .reaching = 0.0};
    double stretches = 0.0;
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        // Weighed down to none at all by the furthest blended, so none drops out with a step.
        constexpr double furthest = 6.0;
        const double weight = maths::exp(-(rough[k] - lowest) / blending) - maths::exp(-furthest);
        if (weight <= 0.0) {
            continue;
        }
        // Turning at each bend halfway between the stretches either side.
        const DVec3 here_way = way(k);
        const DVec3 runs_from = k > 0 ? glm::normalize(way(k - 1) + here_way) : here_way;
        const DVec3 runs_to =
            k + 2 < line.size() ? glm::normalize(here_way + way(k + 1)) : here_way;
        const Wall one = wall_at({.from = line[k],
                                  .to = line[k + 1],
                                  .floor_from = floor_at(runs[k]),
                                  .floor_to = floor_at(runs[k + 1]),
                                  .wall = slope,
                                  .reach = reach,
                                  .head = length > 0.0 ? runs[k] / length : 0.0,
                                  .foot = length > 0.0 ? runs[k + 1] / length : 1.0,
                                  .length = metres,
                                  .belt = channel.belt,
                                  .channel_width = channel.width,
                                  .channel_depth = channel.depth,
                                  .meander = channel.meander,
                                  .meander_length = std::max(channel.meander_length, 1.0),
                                  .seed = hash_values(rock_seed_, point),
                                  .runs_from = runs_from,
                                  .runs_to = runs_to},
                                 direction, radius_);
        stretches += weight;
        add_weighted(wall, one, weight);
    }
    divide(wall, stretches);
    return wall;
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
    Dissected dissected = dissect(
        direction, radius_,
        {.top = {.value = ground.top.value + raised,
                 .gradient = (ground.top.gradient / radius_) + tilt_wide + tilt(narrow)},
         .fall = (ground.fall / radius_) + tilt_wide,
         .above_floor = std::max(0.0, above_floor + raised_wide),
         .above_floor_gradient =
             above_floor + raised_wide > 0.0 ? above_floor_gradient + tilt_wide : DVec3{0.0},
         .steepest = rock_steepest + ((loose_steepest - rock_steepest) * loose),
         .ice = value(ice_)},
        gullies_, lod);
    // Its hollows were worn out of the bed as the gullies were; its spurs stand over it.
    dissected.cut -= raised;
    return dissected;
}

Formed::Here Formed::at(DVec3 direction, double lod) const {
    formation::Smooth::Weights weights;
    smooth_.weigh(direction, weights, start_near(direction));
    const auto value = [&](const std::vector<double>& field) {
        return formation::Smooth::read(field, weights).value;
    };
    Dissected top = surface(direction, weights, lod);

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
    layers.sand = value(sand_);
    layers.alluvium = value(alluvium_);
    layers.lake_beds = value(lake_beds_);
    layers.till = value(till_);
    layers.saprolite = value(saprolite_);
    layers.lava = value(lava_);
    // Gullies cut down through what was laid and into the rock under it, the top of which is
    // what's rotted.
    double cut = std::max(0.0, top.cut);
    for (double* laid : {&layers.loess, &layers.sand, &layers.alluvium, &layers.lake_beds,
                         &layers.till, &layers.lava}) {
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
    // Metres the rock's top rises with its core.
    double rock_rises = 0.0;
    if (rotted > 0.0) {
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
    lay_beach(direction, weights, top.top, layers);
    here.top = top.top;
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
