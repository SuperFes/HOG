#include <hog/planet/erosion.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using hog::planet::Erosion;
using hog::planet::fixed_one;
using hog::planet::LevelGrid;

namespace {

const LevelGrid& grid() {
    static const LevelGrid grid{4, 115'000.0};
    return grid;
}

hog::DVec3 center(std::uint32_t cell) {
    return hog::planet::cell_center(grid().cell(cell));
}

// Land rising towards the north pole, sea south of the equator, roughened.
std::vector<std::int64_t> land() {
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        const hog::DVec3 at = center(cell);
        const double metres =
            at.z > 0.0
                ? 50.0 + (500.0 * at.z) + (40.0 * std::sin(17.0 * at.x) * std::cos(13.0 * at.y))
                : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    return heights;
}

struct Fields {
    std::vector<std::int64_t> uplift;
    std::vector<std::int64_t> erodibility;
    std::vector<std::int64_t> runoff;

    explicit Fields(double uplift_metres, double erodibility_per_step = 0.05)
        : uplift(grid().size(), std::llround(uplift_metres * static_cast<double>(fixed_one))),
          erodibility(grid().size(),
                      std::llround(erodibility_per_step * static_cast<double>(fixed_one))),
          runoff(grid().size(), fixed_one / 2) {}

    [[nodiscard]] Erosion erosion() const {
        return {.uplift = uplift, .erodibility = erodibility, .runoff = runoff};
    }
};

std::vector<std::int64_t> evolve(std::vector<std::int64_t> heights, const Fields& fields,
                                 int steps) {
    for (int step = 0; step < steps; ++step) {
        std::ignore = hog::planet::erode(grid(), heights, fields.erosion());
    }
    return heights;
}

double mean_land(const std::vector<std::int64_t>& heights, auto&& keep) {
    double total = 0.0;
    int count = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        if (heights[cell] > 0 && keep(cell)) {
            total += hog::planet::fixed_to_double(heights[cell]);
            ++count;
        }
    }
    return total / count;
}

} // namespace

TEST_CASE("erosion leaves the sea alone and never cuts land below it", "[planet][erosion]") {
    const auto before = land();
    const auto after = evolve(before, Fields{0.0}, 50);
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        if (before[cell] <= 0) {
            REQUIRE(after[cell] == before[cell]);
        } else {
            REQUIRE(after[cell] >= 0);
        }
    }
}

TEST_CASE("without uplift, rivers only wear the land down", "[planet][erosion]") {
    const auto before = land();
    const auto after = evolve(before, Fields{0.0}, 20);
    bool lowered = false;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        REQUIRE(after[cell] <= before[cell]);
        lowered = lowered || after[cell] < before[cell];
    }
    CHECK(lowered);
}

TEST_CASE("uplift and erosion settle into balance", "[planet][erosion]") {
    const Fields fields{2.0};
    // Valleys only cut down to the lakes they run into, which fill as their floors rise, so it
    // takes a while.
    const auto settled = evolve(land(), fields, 800);
    const auto next = evolve(settled, fields, 1);
    std::int64_t largest = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        largest = std::max(largest, std::abs(next[cell] - settled[cell]));
    }
    CHECK(largest < fixed_one / 50);
}

TEST_CASE("hard rock stands higher than soft once settled", "[planet][erosion]") {
    Fields fields{2.0};
    const auto east = [](std::uint32_t cell) { return center(cell).y > 0.0; };
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        if (east(cell)) {
            fields.erodibility[cell] /= 4;
        }
    }
    const auto settled = evolve(land(), fields, 800);
    const auto west = [&](std::uint32_t cell) { return !east(cell); };
    CHECK(mean_land(settled, east) > 1.5 * mean_land(settled, west));
}

TEST_CASE("wet ground wears faster than dry", "[planet][erosion]") {
    Fields fields{0.0};
    const auto wet = [](std::uint32_t cell) { return center(cell).y > 0.0; };
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        fields.runoff[cell] = wet(cell) ? fixed_one : fixed_one / 20;
    }
    const auto before = land();
    const auto after = evolve(before, fields, 20);
    const auto dry = [&](std::uint32_t cell) { return !wet(cell); };
    const double wet_loss = mean_land(before, wet) - mean_land(after, wet);
    const double dry_loss = mean_land(before, dry) - mean_land(after, dry);
    CHECK(wet_loss > 1.5 * dry_loss);
}
