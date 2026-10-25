#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>
#include <hog/planet/volcanoes.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t layer_salt = 0x766F'6C63'616E'6F00ULL; // "volcano"
constexpr std::uint64_t lobe_salt = 0x6C6F'6265'7300'0000ULL;  // "lobes"

// Each volcano stands in its own cell about this wide, this much of the way in from its edges, so
// none crowds the next and only the cells around a point hold the volcanoes reaching it.
constexpr double site_cell = 20'000.0; // metres
constexpr double margin = 0.2;
// Cells' sides vary about this much over a cube face.
constexpr double cell_spread = 1.5;
constexpr double farthest =
    Volcanoes::max_height / Volcanoes::flank_slope / (1.0 - Volcanoes::lobes);
static_assert(farthest < (1.0 + margin) * (site_cell / 2.0) / cell_spread);
// Lobes around a cone, about; finer octaves cut gullies down it.
constexpr double lobe_frequency = 1.2;
constexpr int lobe_octaves = 4;
// Arcs this weak raise none; stronger, more of the cells along them have one.
constexpr double least_arc = 0.15;
constexpr double full_arc = fixed_to_double(Continents::full_arc);
// The crater's rim lies this much of the way out to the foot, and its floor this much of the
// height below the rim.
constexpr double crater_reach = 0.07;
constexpr double crater_depth = 0.06;

// A uniform double in [0, 1) from the hash's high bits.
double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

// A fraction of the height at a fraction of the way out to the foot: concave, as ash and lava
// pile steepest around the vent.
constexpr double cone(double out) {
    const double in = 1.0 - out;
    return in * (1.0 + in) / 2.0;
}
constexpr double crater_floor = cone(crater_reach) - crater_depth;

} // namespace

Volcanoes::Volcanoes(std::uint64_t seed, double planet_radius, const Continents& continents)
    : planet_radius_{planet_radius}, lobe_seed_{hash_combine(seed, lobe_salt)},
      grid_{level_for(site_cell, planet_radius), planet_radius}, standing_(grid_.size(), none) {
    const std::uint64_t salted = hash_combine(seed, layer_salt);
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const CellId cell = grid_.cell(index);
        const std::uint64_t hash = hash_values(salted, cell.raw());
        const double across = margin + ((1.0 - (2.0 * margin)) * unit(hash_values(hash, 0)));
        const double up = margin + ((1.0 - (2.0 * margin)) * unit(hash_values(hash, 1)));
        const auto corners = cell_vertices(cell);
        const DVec3 center = glm::normalize(glm::mix(glm::mix(corners[0], corners[1], across),
                                                     glm::mix(corners[3], corners[2], across), up));
        const double arc = fixed_to_double(continents.crust(cell_at(center)).arcs);
        const double strength = (arc - least_arc) / (full_arc - least_arc);
        if (unit(hash_values(hash, 2)) >= strength) {
            continue;
        }
        const double height = max_height * (0.5 + (0.5 * unit(hash_values(hash, 3))));
        standing_[index] = static_cast<std::uint32_t>(volcanoes_.size());
        volcanoes_.push_back({.center = center, .height = height, .radius = height / flank_slope});
    }
}

template <int Bits>
std::int64_t Volcanoes::lift(CellId cell) const {
    if (volcanoes_.empty()) {
        return 0;
    }
    const DVec3 at = cell_center(cell);
    const std::uint32_t index = grid_.index(cell_at(at, grid_.level()));
    double highest = 0.0;
    const auto rise = [&](std::uint32_t around) {
        if (standing_[around] == none) {
            return;
        }
        const Volcano& volcano = volcanoes_[standing_[around]];
        const DVec3 off = at - volcano.center;
        const double length = glm::length(off);
        const double reach = length * planet_radius_ / volcano.radius;
        if (reach >= 1.0 / (1.0 - lobes)) {
            return;
        }
        // Read by the way out from the centre alone, so lobes and gullies run straight down.
        const double lobe =
            length > 0.0
                ? fixed_to_double(fbm(hash_values(lobe_seed_, standing_[around]),
                                      noise_point(off / length, lobe_frequency), lobe_octaves))
                : 0.0;
        const double out = reach / (1.0 + (lobes * std::clamp(lobe, -1.0, 1.0)));
        if (out >= 1.0) {
            return;
        }
        const double inside = out / crater_reach;
        const double height =
            out > crater_reach ? cone(out) : crater_floor + (crater_depth * inside * inside);
        highest = std::max(highest, volcano.height * height);
    };
    rise(index);
    for (const std::uint32_t around : grid_.neighbors(index)) {
        rise(around);
    }
    return std::llround(highest * static_cast<double>(fixed_unit<Bits>));
}

template std::int64_t Volcanoes::lift<fixed_fraction_bits>(CellId cell) const;
template std::int64_t Volcanoes::lift<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
