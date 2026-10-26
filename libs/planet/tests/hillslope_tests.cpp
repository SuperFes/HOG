#include <hog/planet/hillslope.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using hog::planet::fixed_one;
using hog::planet::Hillslope;
using hog::planet::LevelGrid;

namespace {

const LevelGrid& grid() {
    static const LevelGrid grid{5, 115'000.0};
    return grid;
}

[[nodiscard]] std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

// A sharp ridge running north to south, falling steeply to either side.
std::vector<std::int64_t> ridge() {
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid().cell(cell));
        heights.push_back(fixed(1'000.0 + (3'000.0 * std::max(0.0, 0.3 - std::abs(at.x)))));
    }
    return heights;
}

struct Fields {
    std::vector<std::int64_t> creep;
    std::vector<std::int64_t> weathering;

    Fields(double creep_square_metres, double weathering_metres)
        : creep(grid().size(), fixed(creep_square_metres)),
          weathering(grid().size(), fixed(weathering_metres)) {}

    [[nodiscard]] Hillslope hillslope() const { return {.creep = creep, .weathering = weathering}; }
};

std::int64_t highest(const std::vector<std::int64_t>& heights) {
    return *std::ranges::max_element(heights);
}

double volume(const std::vector<std::int64_t>& heights) {
    double total = 0.0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        total +=
            hog::planet::fixed_to_double(heights[cell]) * static_cast<double>(grid().area(cell));
    }
    return total;
}

} // namespace

TEST_CASE("bare rock that doesn't weather stays as it is", "[planet][hillslope]") {
    const Fields fields{1.0e6, 0.0};
    std::vector<std::int64_t> heights = ridge();
    std::vector<std::int64_t> soil(grid().size(), 0);
    for (int step = 0; step < 10; ++step) {
        hog::planet::wear_slopes(grid(), heights, soil, fields.hillslope());
    }
    CHECK(heights == ridge());
    CHECK(std::ranges::all_of(soil, [](std::int64_t thick) { return thick == 0; }));
}

TEST_CASE("rock weathers into soil, ever more slowly as it thickens", "[planet][hillslope]") {
    const Fields fields{0.0, 2.0};
    std::vector<std::int64_t> heights = ridge();
    std::vector<std::int64_t> soil(grid().size(), 0);
    hog::planet::wear_slopes(grid(), heights, soil, fields.hillslope());
    const std::int64_t first = soil[0];
    hog::planet::wear_slopes(grid(), heights, soil, fields.hillslope());
    const std::int64_t second = soil[0] - first;
    CHECK(first > fixed_one / 2);
    CHECK(second > 0);
    CHECK(second < first);
    // Rock becomes soil where it lies.
    CHECK(heights == ridge());
}

TEST_CASE("soil creeps off a ridge, rounding it, and none is made or lost", "[planet][hillslope]") {
    const Fields fields{2.0e6, 0.0};
    std::vector<std::int64_t> heights = ridge();
    std::vector<std::int64_t> soil(grid().size(), fixed(100.0));
    for (int step = 0; step < 20; ++step) {
        hog::planet::wear_slopes(grid(), heights, soil, fields.hillslope());
    }
    CHECK(highest(heights) < highest(ridge()) - fixed(10.0));
    CHECK(std::abs(volume(heights) - volume(ridge())) < 1.0e-6 * volume(ridge()));
}

TEST_CASE("creep takes no more soil than there is, so thin soil leaves the rock sharp",
          "[planet][hillslope]") {
    const Fields fields{2.0e6, 0.0};
    std::vector<std::int64_t> heights = ridge();
    std::vector<std::int64_t> thin(grid().size(), fixed(0.2));
    for (int step = 0; step < 20; ++step) {
        hog::planet::wear_slopes(grid(), heights, thin, fields.hillslope());
    }
    CHECK(std::ranges::all_of(thin, [](std::int64_t thick) { return thick >= 0; }));
    CHECK(highest(heights) >= highest(ridge()) - fixed(0.2));

    std::vector<std::int64_t> thick_heights = ridge();
    std::vector<std::int64_t> thick(grid().size(), fixed(100.0));
    for (int step = 0; step < 20; ++step) {
        hog::planet::wear_slopes(grid(), thick_heights, thick, fields.hillslope());
    }
    CHECK(highest(thick_heights) < highest(heights) - fixed(5.0));
}

TEST_CASE("creep quickens beyond proportion as slopes near the steepest the rock holds",
          "[planet][hillslope]") {
    const auto loss = [](double steepest) {
        const Fields fields{2.0e6, 0.0};
        Hillslope hillslope = fields.hillslope();
        hillslope.rock = [&](std::uint32_t, std::int64_t) {
            return hog::planet::Weathering{.rate = fixed_one, .steepest = fixed(steepest)};
        };
        std::vector<std::int64_t> heights = ridge();
        std::vector<std::int64_t> soil(grid().size(), fixed(100.0));
        hog::planet::wear_slopes(grid(), heights, soil, hillslope);
        return highest(ridge()) - highest(heights);
    };
    // The ridge's flanks are about 0.026 steep across these cells.
    CHECK(loss(0.03) > 2 * loss(10.0));
}

TEST_CASE("hard rock that weathers slowly stays bare and sharp beside soft rock that rounds",
          "[planet][hillslope]") {
    const Fields fields{2.0e6, 2.0};
    Hillslope hillslope = fields.hillslope();
    const auto hard = [](std::uint32_t cell) {
        return hog::planet::cell_center(grid().cell(cell)).y > 0.0;
    };
    hillslope.rock = [&](std::uint32_t cell, std::int64_t) {
        return hog::planet::Weathering{.rate = hard(cell) ? 0 : 2 * fixed_one};
    };
    std::vector<std::int64_t> heights = ridge();
    std::vector<std::int64_t> soil(grid().size(), 0);
    for (int step = 0; step < 20; ++step) {
        hog::planet::wear_slopes(grid(), heights, soil, hillslope);
    }
    std::int64_t hard_crest = 0;
    std::int64_t soft_crest = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        (hard(cell) ? hard_crest : soft_crest) =
            std::max(hard(cell) ? hard_crest : soft_crest, heights[cell]);
        // Bare but for traces creeping in from the soft rock.
        if (hog::planet::cell_center(grid().cell(cell)).y > 0.2) {
            CHECK(soil[cell] < fixed_one / 100);
        }
    }
    CHECK(soft_crest < hard_crest - fixed(5.0));
}

TEST_CASE("the sea gives up no soil", "[planet][hillslope]") {
    const Fields fields{2.0e6, 0.0};
    std::vector<std::int64_t> heights = ridge();
    for (std::int64_t& height : heights) {
        height -= fixed(10'000.0);
    }
    const std::vector<std::int64_t> start = heights;
    std::vector<std::int64_t> soil(grid().size(), fixed(100.0));
    hog::planet::wear_slopes(grid(), heights, soil, fields.hillslope());
    CHECK(heights == start);
}

TEST_CASE("ground steeper than its rock holds falls until it isn't, lying loose below",
          "[planet][hillslope]") {
    const Fields fields{0.0, 0.0};
    Hillslope hillslope = fields.hillslope();
    hillslope.rock = [](std::uint32_t, std::int64_t) {
        return hog::planet::Weathering{.rate = fixed_one, .steepest = fixed_one / 2};
    };
    // A tower a cell across, far steeper than the rock stands.
    std::vector<std::int64_t> heights(grid().size(), fixed(100.0));
    const std::uint32_t tower = grid().size() / 3;
    heights[tower] = fixed(5'000.0);
    const std::vector<std::int64_t> start = heights;
    std::vector<std::int64_t> soil(grid().size(), 0);
    hog::planet::wear_slopes(grid(), heights, soil, hillslope);

    CHECK(heights[tower] < fixed(4'000.0));
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        const auto around = grid().neighbors(cell);
        for (std::size_t k = 0; k < around.size(); ++k) {
            const double rise = hog::planet::fixed_to_double(heights[cell] - heights[around[k]]);
            REQUIRE(rise <= (0.5 * hog::planet::fixed_to_double(grid().distance(cell, k))) + 0.1);
        }
    }
    CHECK(std::abs(volume(heights) - volume(start)) < 1.0e-6 * volume(start));
    // What fell lies loose, if not where it first landed.
    double loose = 0.0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        loose += hog::planet::fixed_to_double(soil[cell]) * static_cast<double>(grid().area(cell));
    }
    CHECK(loose > 0.5 * hog::planet::fixed_to_double(start[tower] - heights[tower]) *
                      static_cast<double>(grid().area(tower)));
}
