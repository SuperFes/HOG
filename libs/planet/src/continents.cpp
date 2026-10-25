#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// Keeps this layer's noise independent of every other layer seeded from the same planet seed.
constexpr std::uint64_t layer_salt = 0x636F'6E74'696E'656EULL; // "continen"

constexpr double frequency = 3.0;
constexpr int octaves = 6;
// Shares of the field from the plates' crust and from the noise roughening it.
constexpr std::int64_t crust_share = fixed_one / 2;
constexpr std::int64_t noise_share = fixed_one - crust_share;
// Passes averaging each plate grid cell's crust with its neighbours', spreading the change
// between two plates' crust over about this many cells either side of their boundary squared.
constexpr int crust_passes = 128;

// The crust is read up to this far from where it's asked for, about a third of it typically, along
// smooth bends this long, so the plates' edges and the coasts along them wander instead of running
// straight.
constexpr std::uint64_t warp_salt = 0x7761'7270'0000'0000ULL; // "warp"
constexpr double warp_reach = 25'000.0;                       // metres
constexpr double warp_wavelength = 80'000.0;
constexpr int warp_octaves = 2;

// What plates do to the ground near where they meet, spread over this many passes of smoothing so
// that no seam shows where a cell's nearest boundary changes.
constexpr int tectonic_passes = 8;
// Ranges bend gently, following the boundary's way more smoothly than its wanderings.
constexpr int grain_passes = 24;
// Metres per million years at which plates closing or parting do all they can; slower, they do
// less, but not proportionally less, as even slow collisions pile up ranges over time.
constexpr double full_rate = 25'000.0;
// Where continents collide, a range runs along the boundary this far either side; where a
// continent overrides ocean floor, one rises this far inland and this wide.
constexpr double collision_reach = 30'000.0;
constexpr double coastal_inland = 18'000.0;
constexpr double coastal_reach = 15'000.0;
// Where a continent parts, the ground sinks this much of the field along a rift this wide,
// raising low shoulders either side.
constexpr double rift_depth = 0.3;
constexpr double rift_reach = 10'000.0;
constexpr double shoulder_out = 16'000.0;
constexpr double shoulder_reach = 8'000.0;
constexpr double shoulder_strength = 0.35;
// Where plates part, new floor rises this many metres above the abyss and sinks back to it this far
// out as it cools.
constexpr double ridge_rise = 1'500.0;
constexpr double ridge_reach = 60'000.0;
// Where ocean floor dives under another plate, its trench runs this far out, this wide.
constexpr double trench_out = 6'000.0;
constexpr double trench_reach = 10'000.0;
// The plate it dives under melts above it, raising a line of volcanoes this far in, this wide; out
// at sea, they build the floor under them up this many metres. Even slow plates, or ones diving
// aslant, feed them fully.
constexpr double arc_inland = 20'000.0;
constexpr double arc_reach = 12'000.0;
constexpr std::int64_t arc_rise = 3'000;
constexpr double arc_full_rate = 0.5;

// 1 at `middle`, falling smoothly to 0 at `reach` either side.
double bump(double at, double middle, double reach) {
    const double off = (at - middle) / reach;
    const double inside = std::max(0.0, 1.0 - (off * off));
    return inside * inside;
}

void smooth(const LevelGrid& grid, std::vector<double>& field, int passes) {
    std::vector<double> smoothed(grid.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (std::uint32_t index = 0; index < grid.size(); ++index) {
            double sum = field[index];
            for (const std::uint32_t neighbor : grid.neighbors(index)) {
                sum += field[neighbor];
            }
            smoothed[index] = sum / static_cast<double>(grid.neighbors(index).size() + 1);
        }
        std::swap(field, smoothed);
    }
}

std::vector<std::int64_t> to_fixed(std::span<const double> field) {
    std::vector<std::int64_t> fixed(field.size());
    std::ranges::transform(field, fixed.begin(), [](double value) {
        return std::llround(value * static_cast<double>(fixed_one));
    });
    return fixed;
}

std::vector<std::int64_t> plate_crust(const Plates& plates) {
    std::vector<double> crust(plates.grid().size());
    for (std::uint32_t index = 0; index < crust.size(); ++index) {
        crust[index] = plates.plates()[plates.plate()[index]].continental;
    }
    smooth(plates.grid(), crust, crust_passes);
    return to_fixed(crust);
}

struct Tectonics {
    std::vector<std::int64_t> rifts;     // field lowered
    std::vector<std::int64_t> mountains; // strength from 0 to 1
    std::array<std::vector<std::int64_t>, 3> grain;
    std::vector<std::int64_t> floor; // metres
    std::vector<std::int64_t> arcs;  // strength from 0 to 1
};

Tectonics tectonics(const Plates& plates) {
    const std::size_t size = plates.grid().size();
    std::vector<double> rifts(size, 0.0);
    std::vector<double> mountains(size, 0.0);
    std::vector<double> floor(size, 0.0);
    std::vector<double> arcs(size, 0.0);
    // Squares of the way along the nearest boundary, which have no sign to cancel as they smooth.
    std::array<std::vector<double>, 3> grain;
    std::ranges::fill(grain, std::vector<double>(size, 0.0));
    const auto continental = [&](std::uint32_t plate) {
        return plates.plates()[plate].continental > 0.0;
    };
    for (std::uint32_t index = 0; index < size; ++index) {
        const auto& boundary = plates.boundaries()[index];
        const std::uint32_t own = plates.plate()[index];
        const DVec3 along =
            glm::normalize(glm::cross(cell_center(plates.grid().cell(index)), boundary.normal));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            grain.at(axis)[index] =
                along[static_cast<glm::length_t>(axis)] * along[static_cast<glm::length_t>(axis)];
        }
        const double distance = fixed_to_double(boundary.distance);
        const double pace = std::min(std::abs(boundary.closing) / full_rate, 1.0);
        const double rate = std::copysign(std::sqrt(pace), boundary.closing);
        // Plates sliding past each other neither open a ridge nor dive, however their boundary
        // wobbles.
        const double motion = std::hypot(boundary.closing, boundary.sliding);
        const double head_on =
            motion > 0.0 ? (boundary.closing * boundary.closing) / (motion * motion) : 0.0;
        // Of two oceanic plates, the one first in order dives.
        const bool other_dives =
            !continental(boundary.other) && (continental(own) || boundary.other < own);
        if (rate > 0.0 && other_dives) {
            arcs[index] = std::min(rate / arc_full_rate, 1.0) * std::sqrt(head_on) *
                          bump(distance, arc_inland, arc_reach);
        }
        if (!continental(own)) {
            if (rate < 0.0) {
                const double cooled = std::sqrt(std::min(distance / ridge_reach, 1.0));
                floor[index] = -rate * head_on * ridge_rise * (1.0 - cooled);
            } else if (!other_dives) {
                floor[index] = -rate * head_on * static_cast<double>(Continents::trench_depth) *
                               bump(distance, trench_out, trench_reach);
            }
            continue;
        }
        if (rate > 0.0) {
            mountains[index] = rate * (continental(boundary.other)
                                           ? bump(distance, 0.0, collision_reach)
                                           : bump(distance, coastal_inland, coastal_reach));
        } else if (continental(boundary.other)) {
            rifts[index] = -rate * rift_depth * bump(distance, 0.0, rift_reach);
            mountains[index] =
                -rate * shoulder_strength * bump(distance, shoulder_out, shoulder_reach);
        }
    }
    smooth(plates.grid(), rifts, tectonic_passes);
    smooth(plates.grid(), mountains, tectonic_passes);
    smooth(plates.grid(), floor, tectonic_passes);
    smooth(plates.grid(), arcs, tectonic_passes);
    Tectonics made{.rifts = to_fixed(rifts),
                   .mountains = to_fixed(mountains),
                   .grain = {},
                   .floor = to_fixed(floor),
                   .arcs = to_fixed(arcs)};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        smooth(plates.grid(), grain.at(axis), grain_passes);
        made.grain.at(axis) = to_fixed(grain.at(axis));
    }
    return made;
}

} // namespace

Continents::Continents(std::uint64_t seed, double ocean_fraction,
                       std::shared_ptr<const Plates> plates)
    : seed_{hash_combine(seed, layer_salt)}, warp_seed_{hash_combine(seed, warp_salt)},
      plates_{std::move(plates)}, crust_{plate_crust(*plates_)} {
    Tectonics made = tectonics(*plates_);
    rifts_ = std::move(made.rifts);
    mountains_ = std::move(made.mountains);
    grain_ = std::move(made.grain);
    floor_ = std::move(made.floor);
    arcs_ = std::move(made.arcs);
    HOG_ASSERT(ocean_fraction >= 0.0 && ocean_fraction < 1.0, "ocean fraction must be in [0, 1)");

    std::vector<std::int64_t> survey;
    for (int face = 0; face < face_count; ++face) {
        const auto samples = sample_tile(CellId::from_face(face), survey_level,
                                         [this](CellId cell) { return field(cell); });
        survey.insert(survey.end(), samples.begin(), samples.end());
    }

    // The ocean is the `ocean_cells` lowest samples; land must rise above the highest of them.
    const auto ocean_cells =
        static_cast<std::size_t>(std::llround(ocean_fraction * static_cast<double>(survey.size())));
    if (ocean_cells == 0) {
        sea_level_ = *std::ranges::min_element(survey) - 1;
        return;
    }
    const auto highest_ocean = survey.begin() + static_cast<std::ptrdiff_t>(ocean_cells - 1);
    std::ranges::nth_element(survey, highest_ocean);
    sea_level_ = *highest_ocean;
}

CellId Continents::warped(CellId cell) const {
    const double radius = plates_->radius();
    const DVec3 center = cell_center(cell);
    const NoisePoint at = noise_point(center, radius / warp_wavelength);
    const DVec3 shift{fixed_to_double(fbm(warp_seed_, at, warp_octaves)),
                      fixed_to_double(fbm(warp_seed_ + 1, at, warp_octaves)),
                      fixed_to_double(fbm(warp_seed_ + 2, at, warp_octaves))};
    return cell_at(glm::normalize(center + (shift * (warp_reach / radius))));
}

template <int Bits>
std::int64_t Continents::field(CellId cell, CellId warped) const {
    constexpr int finer = Bits - fixed_fraction_bits;
    const std::int64_t noise = fbm(seed_, noise_point<Bits>(cell_center(cell), frequency), octaves);
    const std::int64_t plate = (plates_->grid().sample(crust_, warped) << finer) -
                               (plates_->grid().sample(rifts_, warped) << finer);
    return ((noise * noise_share) + (plate * crust_share)) >> fixed_fraction_bits;
}

template <int Bits>
std::int64_t Continents::field(CellId cell) const {
    return field<Bits>(cell, warped(cell));
}

template <int Bits>
Continents::Crust Continents::crust(CellId cell) const {
    const CellId at = warped(cell);
    constexpr int finer = Bits - fixed_fraction_bits;
    const std::int64_t arcs =
        std::clamp<std::int64_t>(plates_->grid().sample(arcs_, at), 0, fixed_one);
    // Built up fully wherever volcanoes rise all along the arc.
    const std::int64_t platform = (arc_rise * std::min(arcs, full_arc) * fixed_one) / full_arc;
    Crust crust{.height = field<Bits>(cell, at) - (sea_level_ << finer),
                .mountains =
                    std::clamp<std::int64_t>(plates_->grid().sample(mountains_, at), 0, fixed_one)
                    << finer,
                .grain = {},
                .floor = (plates_->grid().sample(floor_, at) + platform) << finer,
                .arcs = arcs << finer};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        crust.grain.at(axis) =
            std::max<std::int64_t>(plates_->grid().sample(grain_.at(axis), at), 0);
    }
    return crust;
}

template std::int64_t Continents::field<fixed_fraction_bits>(CellId cell) const;
template std::int64_t Continents::field<fine_fraction_bits>(CellId cell) const;
template Continents::Crust Continents::crust<fixed_fraction_bits>(CellId cell) const;
template Continents::Crust Continents::crust<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
