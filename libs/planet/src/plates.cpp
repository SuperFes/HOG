#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/plates.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <queue>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t origin_salt = 0x6F72'6967'696E'0000ULL; // "origin"
constexpr std::uint64_t spin_salt = 0x7370'696E'0000'0000ULL;   // "spin"
constexpr std::uint64_t growth_salt = 0x6772'6F77'7468'0000ULL; // "growth"
constexpr std::uint64_t rough_salt = 0x726F'7567'6800'0000ULL;  // "rough"

// Earth's plates move 2 to 8 cm a year.
constexpr double slowest = 20'000.0; // metres per million years
constexpr double fastest = 80'000.0;
// Origins keep at least this fraction of the spacing evenly spread plates would have.
constexpr double origin_spacing = 0.5;
constexpr int origin_attempts = 1'000;
// The ground's resistance to a plate spreading varies by this much either way, so boundaries
// wander instead of running straight between origins.
constexpr double rough_frequency = 10.0;
constexpr int rough_octaves = 5;
constexpr std::int64_t rough_spread = (fixed_one * 9) / 10;
// Passes averaging each boundary's direction with its neighbours', so the grid's steps don't
// show in how the plates meet.
constexpr int normal_passes = 3;

// Cheapest first; ties go to the lower cell, so the order never depends on the heap.
using Entry = std::tuple<std::int64_t, std::uint32_t, std::uint32_t>;
using Frontier = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;

// A uniform double in [0, 1) from the hash's high bits.
[[nodiscard]] double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

// A unit vector uniform over the sphere, by rejection so no trigonometry is needed.
[[nodiscard]] DVec3 direction(std::uint64_t seed) {
    for (std::uint64_t attempt = 0;; ++attempt) {
        const DVec3 point{(2.0 * unit(hash_values(seed, attempt, 0U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 1U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 2U))) - 1.0};
        const double length = glm::length(point);
        if (length <= 1.0 && length > 0.01) {
            return point / length;
        }
    }
}

[[nodiscard]] DVec3 tangent(DVec3 vector, DVec3 at) {
    return vector - (at * glm::dot(vector, at));
}

[[nodiscard]] std::vector<Plates::Plate> random_plates(std::uint64_t seed, double radius,
                                                       int count) {
    HOG_ASSERT(count >= 2 && count <= Plates::max_count, "plate count out of range");
    const double spacing =
        origin_spacing * std::sqrt(4.0 * std::numbers::pi / static_cast<double>(count));
    std::vector<Plates::Plate> plates;
    std::uint64_t draw = 0;
    while (plates.size() < static_cast<std::size_t>(count)) {
        const auto index = static_cast<std::uint64_t>(plates.size());
        DVec3 origin{};
        for (int attempt = 0; attempt < origin_attempts; ++attempt, ++draw) {
            origin = direction(hash_values(seed, origin_salt, draw));
            const bool apart = std::ranges::all_of(plates, [&](const Plates::Plate& plate) {
                return glm::length(plate.origin - origin) >= spacing;
            });
            if (apart) {
                break;
            }
        }
        const double speed =
            slowest + ((fastest - slowest) * unit(hash_values(seed, spin_salt, index, 3U)));
        // Skewed so a few plates grow large among many small ones, as on Earth.
        const double grows = unit(hash_values(seed, growth_salt, index));
        plates.push_back({.origin = origin,
                          .spin = direction(hash_values(seed, spin_salt, index)) * (speed / radius),
                          .growth = 0.7 + (0.7 * grows * grows)});
    }
    return plates;
}

// Where a grid cell meets another plate: the plate it meets most, and the way towards it.
struct Edge {
    std::uint32_t other;
    DVec3 normal;
};
constexpr auto inland = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] std::vector<Edge> find_edges(const LevelGrid& grid,
                                           std::span<const std::uint32_t> plates) {
    std::vector<Edge> edges(grid.size(), {.other = inland, .normal = {}});
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        std::array<int, Plates::max_count> across{};
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            if (plates[neighbor] != plates[index]) {
                ++across[plates[neighbor]];
            }
        }
        const auto other =
            static_cast<std::uint32_t>(std::ranges::max_element(across) - across.begin());
        if (across[other] == 0) {
            continue;
        }
        const DVec3 at = cell_center(grid.cell(index));
        DVec3 normal{};
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            if (plates[neighbor] == other) {
                normal += glm::normalize(tangent(cell_center(grid.cell(neighbor)) - at, at));
            }
        }
        edges[index] = {.other = other, .normal = normal};
    }
    return edges;
}

// Each edge's way across summed with its neighbours' along the same boundary.
[[nodiscard]] std::vector<Edge> smooth(const LevelGrid& grid, std::span<const std::uint32_t> plates,
                                       std::span<const Edge> edges) {
    std::vector<Edge> smoothed{edges.begin(), edges.end()};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (edges[index].other == inland) {
            continue;
        }
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            if (plates[neighbor] == plates[index] && edges[neighbor].other == edges[index].other) {
                smoothed[index].normal += edges[neighbor].normal;
            }
        }
    }
    return smoothed;
}

} // namespace

Plates::Plates(std::uint64_t seed, LevelGrid grid, double radius, int count)
    : Plates{seed, std::move(grid), radius, random_plates(seed, radius, count)} {}

Plates::Plates(std::uint64_t seed, LevelGrid grid, double radius, std::vector<Plate> plates)
    : grid_{std::move(grid)}, radius_{radius}, plates_{std::move(plates)}, plate_(grid_.size(), 0),
      boundaries_(grid_.size()) {
    HOG_ASSERT(plates_.size() >= 2 && plates_.size() <= max_count, "plate count out of range");
    grow(seed);
    find_boundaries();
}

DVec3 Plates::velocity(std::uint32_t plate, DVec3 at) const {
    return glm::cross(plates_[plate].spin, at) * radius_;
}

void Plates::grow(std::uint64_t seed) {
    const std::uint64_t rough_seed = hash_combine(seed, rough_salt);
    std::vector<std::int64_t> rough(grid_.size());
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const auto noise =
            fbm(rough_seed, noise_point(cell_center(grid_.cell(index)), rough_frequency),
                rough_octaves);
        rough[index] = fixed_one + ((noise * rough_spread) >> fixed_fraction_bits);
    }
    std::vector<std::int64_t> growth(plates_.size());
    std::ranges::transform(plates_, growth.begin(), [](const Plate& plate) {
        return std::llround(plate.growth * static_cast<double>(fixed_one));
    });

    constexpr auto unclaimed = std::numeric_limits<std::uint32_t>::max();
    std::ranges::fill(plate_, unclaimed);
    Frontier frontier;
    for (std::uint32_t plate = 0; plate < plates_.size(); ++plate) {
        frontier.emplace(0, grid_.index(cell_at(plates_[plate].origin, grid_.level())), plate);
    }
    while (!frontier.empty()) {
        const auto [cost, index, plate] = frontier.top();
        frontier.pop();
        if (plate_[index] != unclaimed) {
            continue;
        }
        plate_[index] = plate;
        const auto around = grid_.neighbors(index);
        const auto distances = grid_.distances(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            if (plate_[around[k]] == unclaimed) {
                const std::int64_t step = (distances[k] * rough[around[k]]) / growth[plate];
                frontier.emplace(cost + step, around[k], plate);
            }
        }
    }
}

void Plates::find_boundaries() {
    std::vector<Edge> edges = find_edges(grid_, plate_);
    for (int pass = 0; pass < normal_passes; ++pass) {
        edges = smooth(grid_, plate_, edges);
    }
    Frontier frontier;
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const Edge& edge = edges[index];
        if (edge.other == inland) {
            continue;
        }
        const DVec3 at = cell_center(grid_.cell(index));
        const DVec3 normal = glm::normalize(tangent(edge.normal, at));
        const DVec3 relative = velocity(plate_[index], at) - velocity(edge.other, at);
        const double closing = glm::dot(relative, normal);
        boundaries_[index] = {.distance = 0,
                              .other = edge.other,
                              .closing = closing,
                              .sliding = glm::length(relative - (normal * closing))};
        frontier.emplace(0, index, index);
    }
    std::vector<bool> reached(grid_.size(), false);
    while (!frontier.empty()) {
        const auto [distance, index, from] = frontier.top();
        frontier.pop();
        if (reached[index]) {
            continue;
        }
        reached[index] = true;
        boundaries_[index] = boundaries_[from];
        boundaries_[index].distance = distance;
        const auto around = grid_.neighbors(index);
        const auto distances = grid_.distances(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            // Each plate's ground measures to its own edge.
            if (!reached[around[k]] && plate_[around[k]] == plate_[index]) {
                frontier.emplace(distance + distances[k], around[k], from);
            }
        }
    }
}

} // namespace hog::planet
