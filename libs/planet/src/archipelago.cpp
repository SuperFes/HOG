#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/archipelago.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <tuple>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t placement_salt = 0x6172'6368'6970'656CULL; // "archipel"
constexpr std::uint64_t coast_salt = 0x636F'6173'7473'0000ULL;     // "coasts"

// Sine of the latitudes bounding the temperate band the islands prefer: 38 and 46 degrees.
constexpr double band_low = 0.615661475325658;
constexpr double band_high = 0.719339800338651;

// Candidate centres are the cells at this level, and the ocean around a centre must stay this far
// below sea level at every probe.
constexpr int candidate_level = 4;
constexpr std::int64_t open_ocean = -fixed_one / 20;

constexpr std::int64_t min_peak = fixed_one / 10;
constexpr std::int64_t max_peak = fixed_one / 5;
constexpr int placement_attempts = 10'000;

constexpr double coast_wavelength = 4'000.0; // metres
constexpr int coast_octaves = 5;
// Fractal noise rarely strays far from zero, so it is scaled up and clipped to [-1, 1].
constexpr double coast_gain = 2.5;
// The coastline noise moves an island's coast by up to this fraction of its radius either way.
constexpr double coast_wobble = 0.4;
// Each island keeps this many times its undisturbed extent clear, for the coastline noise.
constexpr double spread = 1.0 + coast_wobble;
// Continent height lost per metre down an island's underwater flank: about 15 degrees.
constexpr double skirt_slope = 1e-5;

using Island = Archipelago::Island;

// A uniform double in [0, 1) from the hash's high bits.
[[nodiscard]] double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

[[nodiscard]] DVec3 offset(DVec3 origin, DVec3 tangent, double angle) {
    return glm::normalize(origin + (tangent * angle));
}

// A unit vector uniform in the plane, as x and y, by rejection so no trigonometry is needed.
template <typename Draw>
[[nodiscard]] std::optional<std::array<double, 2>> planar_direction(Draw draw) {
    const double x = (2.0 * draw(0)) - 1.0;
    const double y = (2.0 * draw(1)) - 1.0;
    const double length = std::sqrt((x * x) + (y * y));
    if (length > 1.0 || length < 0.01) {
        return std::nullopt;
    }
    return std::array{x / length, y / length};
}

// The length of an offset from an island's centre, squeezed along the island's axis and stretched
// across it so that the undisturbed coast lies at the island's radius all round.
[[nodiscard]] double stretched(const Island& island, DVec3 offset) {
    const double root = std::sqrt(island.elongation);
    const double along = glm::dot(offset, island.axis) / root;
    const double across = glm::dot(offset, glm::cross(island.center, island.axis)) * root;
    return std::sqrt((along * along) + (across * across));
}

// Metres from an island's centre to its undisturbed coast in the direction of an offset.
[[nodiscard]] double extent(const Island& island, DVec3 offset) {
    return island.radius * glm::length(offset) / stretched(island, offset);
}

// A tangent at `center` pointing the way `direction` does.
[[nodiscard]] DVec3 tangent_at(DVec3 center, DVec3 direction) {
    return glm::normalize(direction - (center * glm::dot(direction, center)));
}

// Sixteen probes on two rings, using exact compass directions so every platform agrees.
[[nodiscard]] std::int64_t highest_nearby(const Continents& continents, DVec3 center,
                                          double angle) {
    const TangentFrame frame = tangent_frame(center);
    const double diagonal = std::sqrt(0.5);
    const std::array<DVec3, 8> directions{
        frame.east,   (frame.east + frame.north) * diagonal,
        frame.north,  (frame.north - frame.east) * diagonal,
        -frame.east,  (-frame.east - frame.north) * diagonal,
        -frame.north, (frame.east - frame.north) * diagonal,
    };
    std::int64_t highest = continents.height(cell_at(center));
    for (const double fraction : {0.5, 1.0}) {
        for (const DVec3 direction : directions) {
            highest = std::max(
                highest, continents.height(cell_at(offset(center, direction, angle * fraction))));
        }
    }
    return highest;
}

// The open ocean nearest the temperate band, or failing any, the most open spot; ties go to the
// lowest hash so the choice is seeded.
[[nodiscard]] DVec3 choose_center(std::uint64_t seed, double planet_radius,
                                  const Continents& continents) {
    struct Candidate {
        bool open = false;
        double off_band = 0.0;
        std::int64_t highest = 0;
        std::uint64_t hash = 0;
        DVec3 center{};
    };
    const auto key = [](const Candidate& c) {
        return std::tuple{!c.open, c.off_band, c.highest, c.hash};
    };
    std::vector<Candidate> candidates;
    for (int face = 0; face < face_count; ++face) {
        for (const CellId cell : sample_tile(CellId::from_face(face), candidate_level,
                                             [](CellId candidate) { return candidate; })) {
            const DVec3 center = cell_center(cell);
            const std::int64_t highest =
                highest_nearby(continents, center, Archipelago::clearance / planet_radius);
            const double latitude = std::abs(center.z);
            const bool open = highest < open_ocean;
            candidates.push_back(
                {.open = open,
                 .off_band = std::max({band_low - latitude, latitude - band_high, 0.0}),
                 .highest = open ? 0 : highest,
                 .hash = hash_values(seed, cell.raw()),
                 .center = center});
        }
    }
    return std::ranges::min(candidates, {}, key).center;
}

} // namespace

Archipelago::Archipelago(std::uint64_t seed, double planet_radius, const Continents& continents)
    : planet_radius_{planet_radius}, noise_seed_{hash_combine(seed, coast_salt)},
      // Continent heights never fall below -1 - sea level, so a flank is buried past this.
      skirt_reach_{(1.0 + fixed_to_double(continents.sea_level())) / skirt_slope} {
    HOG_ASSERT(planet_radius > clearance, "planet too small for the starting islands");
    const std::uint64_t placement = hash_combine(seed, placement_salt);
    center_ = choose_center(placement, planet_radius, continents);

    const std::size_t count =
        min_islands + (hash_values(placement, 0) % (max_islands - min_islands + 1));
    islands_.reserve(count);
    const TangentFrame home = tangent_frame(center_);
    for (std::size_t index = 0; index < count; ++index) {
        const double area = min_target_area + ((max_target_area - min_target_area) *
                                               unit(hash_values(placement, 1, index)));
        std::optional<std::array<double, 2>> heading;
        for (std::uint64_t attempt = 0; !heading; ++attempt) {
            heading = planar_direction([&](std::uint64_t what) {
                return unit(hash_values(placement, 5, index, attempt, what));
            });
        }
        const DVec3 axis = (home.east * (*heading)[0]) + (home.north * (*heading)[1]);
        const Island shape{
            .center = center_,
            .radius = std::sqrt(area / std::numbers::pi),
            .axis = tangent_at(center_, axis),
            .elongation = 1.0 + ((max_elongation - 1.0) * unit(hash_values(placement, 6, index))),
            .peak = min_peak + static_cast<std::int64_t>(hash_values(placement, 2, index) %
                                                         (max_peak - min_peak + 1))};
        if (islands_.empty()) {
            islands_.push_back(shape);
            continue;
        }
        const auto metres = [&](DVec3 a, DVec3 b) { return glm::length(a - b) * planet_radius; };
        const double longest = shape.radius * std::sqrt(shape.elongation);
        bool placed = false;
        for (std::uint64_t attempt = 0; attempt < placement_attempts && !placed; ++attempt) {
            const Island& anchor =
                islands_[hash_values(placement, 4, index, attempt) % islands_.size()];
            const auto direction = planar_direction([&](std::uint64_t what) {
                return unit(hash_values(placement, 3, index, attempt, what));
            });
            if (!direction) {
                continue;
            }
            const TangentFrame frame = tangent_frame(anchor.center);
            const DVec3 outward = (frame.east * (*direction)[0]) + (frame.north * (*direction)[1]);
            // Sized as if it sat on the anchor, which is close enough to choose the distance.
            Island island = shape;
            island.center = anchor.center;
            island.axis = tangent_at(anchor.center, axis);
            const double gap = min_gap + ((max_gap - min_gap) *
                                          unit(hash_values(placement, 3, index, attempt, 2)));
            const double distance =
                (spread * (extent(anchor, outward) + extent(island, outward))) + gap;
            island.center = offset(anchor.center, outward, distance / planet_radius);
            island.axis = tangent_at(island.center, axis);
            if (metres(island.center, center_) + (spread * longest) > reach) {
                continue;
            }
            const bool clear = std::ranges::all_of(islands_, [&](const Island& other) {
                const DVec3 between = island.center - other.center;
                return metres(island.center, other.center) >=
                       (spread * (extent(other, between) + extent(island, between))) + min_gap;
            });
            if (clear) {
                islands_.push_back(island);
                placed = true;
            }
        }
        HOG_ASSERT(placed, "no room for another starting island");
    }
}

template <int Bits>
std::int64_t Archipelago::raise(CellId cell, std::int64_t height) const {
    constexpr std::int64_t one = fixed_unit<Bits>;
    const DVec3 position = cell_center(cell);
    // The stretched distance is at least the true one over the root of the elongation, and the
    // noise moves the coast out by at most the wobble, so past this every flank is buried.
    const auto horizon = [&](double radius, double elongation) {
        return std::sqrt(elongation) * ((radius * (1.0 + coast_wobble)) + skirt_reach_);
    };
    const double largest = std::sqrt(max_target_area / std::numbers::pi);
    if (glm::length(position - center_) * planet_radius_ >
        reach + horizon(largest, max_elongation)) {
        return height;
    }
    std::optional<double> wobble;
    for (const Island& island : islands_) {
        const DVec3 between = (position - island.center) * planet_radius_;
        if (glm::length(between) > horizon(island.radius, island.elongation)) {
            continue;
        }
        if (!wobble) {
            wobble = coast_wobble *
                     std::clamp(coast_gain *
                                    fixed_to_double(fbm(
                                        noise_seed_,
                                        noise_point(position, planet_radius_ / coast_wavelength),
                                        coast_octaves)),
                                -1.0, 1.0);
        }
        // The coast moves out to the radius times 1 + wobble: the dome is scaled to it, while the
        // flank is shifted so it keeps its slope however far out it reaches.
        const double distance = stretched(island, between);
        const double coast = island.radius * (1.0 + *wobble);
        const double from_center =
            distance < coast ? distance * island.radius / coast : distance - coast + island.radius;
        std::int64_t island_height = 0;
        if (from_center < island.radius) {
            const std::int64_t ratio =
                std::llround(from_center / island.radius * static_cast<double>(one));
            const std::int64_t falloff = (ratio * ratio) >> Bits;
            const std::int64_t peak = island.peak << (Bits - fixed_fraction_bits);
            island_height = peak - ((peak * falloff) >> Bits);
        } else {
            island_height = -std::llround((from_center - island.radius) * skirt_slope *
                                          static_cast<double>(one));
        }
        height = std::max(height, island_height);
    }
    return height;
}

template std::int64_t Archipelago::raise<fixed_fraction_bits>(CellId cell,
                                                              std::int64_t height) const;
template std::int64_t Archipelago::raise<fine_fraction_bits>(CellId cell,
                                                             std::int64_t height) const;

} // namespace hog::planet
