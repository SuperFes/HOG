#include <hog/core/hash.hpp>
#include <hog/planet/hotspots.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t layer_salt = 0x686F'7473'706F'7473ULL; // "hotspots"
constexpr std::uint64_t lobe_salt = 0x7269'6674'7300'0000ULL;  // "rifts"
constexpr std::uint64_t rough_salt = 0x726F'7567'6800'0000ULL; // "rough"
constexpr int attempts = 512;

// Plumes rise under deep, open ocean, well inside a plate, this far apart.
constexpr std::int64_t deep_ocean = -fixed_one / 5;
constexpr double inside_plate = 30'000.0; // metres
constexpr double apart = 150'000.0;
// Every shield keeps this far from the place to avoid.
constexpr double clear_of = 60'000.0;

// A new shield grows this far along the chain from the last, give or take this much across it.
constexpr double spacing = 12'000.0;
constexpr double wander = 0.15;
// Metres a shield has sunk by the time the next has grown, give or take this many.
constexpr double subsidence = 450.0;
constexpr double subsidence_spread = 150.0;

// A shield's flank falls this many metres per metre near the summit, steepening by this much per
// metre out, as lava runs far before it piles up; past `reach` it lies below any floor around.
constexpr double top_slope = 0.05;
constexpr double steepening = 4.3e-5;
constexpr double reach = 16'000.0;
static_assert((Hotspots::young_summit - (top_slope * reach) - (steepening / 2.0 * reach * reach)) <
              -5'000.0);
// Lava pours furthest down a few rift zones, pushing the flanks out, or pulling them in, up to this
// much of the way.
constexpr double lobes = 0.3;
constexpr double lobe_frequency = 0.8;
constexpr int lobe_octaves = 3;
// Flows and gullies roughen the flanks below the summit by up to about this many metres over about
// this far, so the coasts wander.
constexpr double roughness = 40.0;
constexpr double smooth_summit = 1'000.0;
constexpr double rough_wavelength = 1'500.0;
constexpr int rough_octaves = 3;
// The caldera at the summit.
constexpr double caldera_radius = 800.0;
constexpr double caldera_depth = 150.0;
// The swell spreads this far around a plume.
constexpr double swell_reach = 40'000.0;

// A uniform double in [0, 1) from the hash's high bits.
double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

// A unit direction uniform over the sphere, by rejection so no trigonometry is needed.
std::optional<DVec3> direction(std::uint64_t hash) {
    const DVec3 inside{(2.0 * unit(hash_values(hash, 0))) - 1.0,
                       (2.0 * unit(hash_values(hash, 1))) - 1.0,
                       (2.0 * unit(hash_values(hash, 2))) - 1.0};
    const double length = glm::length(inside);
    if (length > 1.0 || length < 0.01) {
        return std::nullopt;
    }
    return inside / length;
}

DVec3 tangent(DVec3 at, DVec3 way) {
    return way - (at * glm::dot(way, at));
}

// 1 at the middle, falling smoothly to 0 at `width`.
double bump(double at, double width) {
    const double off = at / width;
    const double inside = std::max(0.0, 1.0 - (off * off));
    return inside * inside;
}

} // namespace

Hotspots::Hotspots(std::uint64_t seed, double planet_radius, const Continents& continents,
                   DVec3 avoid)
    : planet_radius_{planet_radius}, lobe_seed_{hash_combine(seed, lobe_salt)},
      rough_seed_{hash_combine(seed, rough_salt)} {
    const std::uint64_t salted = hash_combine(seed, layer_salt);
    const Plates& plates = continents.plates();
    const auto metres = [&](DVec3 from, DVec3 to) {
        return glm::length(to - from) * planet_radius;
    };
    for (int attempt = 0; attempt < attempts && std::ssize(hotspots_) < count; ++attempt) {
        const std::uint64_t hash = hash_values(salted, attempt);
        const auto center = direction(hash);
        if (!center || continents.crust(cell_at(*center)).height >= deep_ocean ||
            std::ranges::any_of(hotspots_, [&](const Hotspot& other) {
                return metres(other.center, *center) < apart;
            })) {
            continue;
        }
        const std::uint32_t index = plates.grid().index(cell_at(*center, plates.grid().level()));
        if (fixed_to_double(plates.boundaries()[index].distance) < inside_plate) {
            continue;
        }
        const DVec3 moving = tangent(*center, plates.velocity(plates.plate()[index], *center));
        const DVec3 heading =
            glm::length(moving) > 1.0 ? glm::normalize(moving) : tangent_frame(*center).east;

        // Carried along step by step, keeping to the sphere without trigonometry.
        std::vector<Shield> chain;
        DVec3 along = *center;
        DVec3 way = heading;
        double summit = young_summit;
        for (int age = 0; age < chain_length; ++age) {
            const std::uint64_t shield = hash_values(hash, 3, age);
            const DVec3 across = glm::cross(along, way);
            const double aside = wander * ((2.0 * unit(hash_values(shield, 0))) - 1.0);
            chain.push_back(
                {.center = glm::normalize(along + (across * (aside * spacing / planet_radius))),
                 .summit = summit});
            along = glm::normalize(along + (way * (spacing / planet_radius)));
            way = glm::normalize(tangent(along, way));
            summit -=
                subsidence + (subsidence_spread * ((2.0 * unit(hash_values(shield, 1))) - 1.0));
        }
        if (std::ranges::any_of(chain, [&](const Shield& shield) {
                return metres(shield.center, avoid) < clear_of;
            })) {
            continue;
        }
        hotspots_.push_back({.center = *center, .heading = heading});
        shields_.insert(shields_.end(), chain.begin(), chain.end());
    }
}

template <int Bits>
std::int64_t Hotspots::swell(CellId cell) const {
    const DVec3 at = cell_center(cell);
    double raised = 0.0;
    for (const Hotspot& hotspot : hotspots_) {
        raised +=
            swell_height * bump(glm::length(at - hotspot.center) * planet_radius_, swell_reach);
    }
    return std::llround(raised * static_cast<double>(fixed_unit<Bits>));
}

template <int Bits>
std::optional<std::int64_t> Hotspots::surface(CellId cell) const {
    const DVec3 at = cell_center(cell);
    std::optional<double> highest;
    double rough = 0.0;
    for (std::size_t index = 0; index < shields_.size(); ++index) {
        const Shield& shield = shields_[index];
        const DVec3 off = at - shield.center;
        const double length = glm::length(off);
        if (length * planet_radius_ >= reach / (1.0 - lobes)) {
            continue;
        }
        // Read by the way out from the centre alone, so rift zones run straight down.
        const double lobe =
            length > 0.0
                ? fixed_to_double(fbm(hash_values(lobe_seed_, index),
                                      noise_point(off / length, lobe_frequency), lobe_octaves))
                : 0.0;
        const double out = length * planet_radius_ / (1.0 + (lobes * std::clamp(lobe, -1.0, 1.0)));
        if (out >= reach) {
            continue;
        }
        // The caldera stays round.
        const double from = length * planet_radius_;
        const double pit = std::max(0.0, 1.0 - ((from * from) / (caldera_radius * caldera_radius)));
        const double height = shield.summit - (top_slope * out) - (steepening / 2.0 * out * out) -
                              (caldera_depth * pit);
        if (!highest || height > *highest) {
            highest = height;
            rough = std::clamp((from / smooth_summit) - 1.0, 0.0, 1.0);
        }
    }
    if (!highest) {
        return std::nullopt;
    }
    rough *= roughness *
             fixed_to_double(fbm(rough_seed_, noise_point(at, planet_radius_ / rough_wavelength),
                                 rough_octaves));
    return std::llround((*highest + rough) * static_cast<double>(fixed_unit<Bits>));
}

template std::int64_t Hotspots::swell<fixed_fraction_bits>(CellId cell) const;
template std::int64_t Hotspots::swell<fine_fraction_bits>(CellId cell) const;
template std::optional<std::int64_t> Hotspots::surface<fixed_fraction_bits>(CellId cell) const;
template std::optional<std::int64_t> Hotspots::surface<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
