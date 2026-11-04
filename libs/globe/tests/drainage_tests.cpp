#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace {

constexpr int level = 4;
constexpr double radius = 1'000.0;

// Ground by direction: high to the north, sinking to the sea in the south.
std::vector<float> slope(const auto& height_at) {
    std::vector<float> ground(static_cast<std::size_t>(hog::globe::cells_at(level)));
    for (std::size_t cell = 0; cell < ground.size(); ++cell) {
        const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(level, cell));
        ground[cell] = static_cast<float>(height_at(up, cell));
    }
    return ground;
}

} // namespace

TEST_CASE("every cell's water reaches the sea, and all the ground drains through it",
          "[globe][drainage]") {
    const auto ground = slope([](hog::DVec3 up, std::size_t) { return 100.0 * up.z; });
    const hog::globe::Drainage drained = hog::globe::drain(ground, level, radius, 0.0);
    double sea_catchment = 0.0;
    for (std::uint32_t cell = 0; cell < ground.size(); ++cell) {
        std::uint32_t at = cell;
        std::size_t steps = 0;
        while (drained.receiver[at] != at) {
            at = drained.receiver[at];
            REQUIRE(++steps < ground.size());
        }
        CHECK(ground[at] <= 0.0F);
        if (drained.receiver[cell] == cell) {
            sea_catchment += drained.catchment[cell];
        }
    }
    CHECK(sea_catchment == Catch::Approx(4.0 * std::numbers::pi * radius * radius).epsilon(1e-6));
}

TEST_CASE("a hollow in high ground fills to its rim and spills", "[globe][drainage]") {
    std::size_t pit = 0;
    while (hog::globe::center(hog::globe::cell_at_index(level, pit)).z < 0.5) {
        ++pit;
    }
    const auto ground = slope([&](hog::DVec3 up, std::size_t cell) {
        return up.z > 0.2 ? 100.0 + (cell == pit ? -20.0 : 0.0) : (100.0 * up.z) - 20.0;
    });
    REQUIRE(ground[pit] < 100.0F);
    const hog::globe::Drainage drained = hog::globe::drain(ground, level, radius, 0.0);
    CHECK(drained.depth(static_cast<std::uint32_t>(pit)) > 19.0);
    for (std::uint32_t cell = 0; cell < ground.size(); ++cell) {
        CHECK(drained.filled[cell] >= ground[cell]);
    }
}

TEST_CASE("running water only lowers ground, most where most drains, and leaves the sea",
          "[globe][drainage]") {
    const auto ground = slope(
        [](hog::DVec3 up, std::size_t) { return (100.0 * up.z) + (5.0 * std::sin(40.0 * up.x)); });
    const hog::globe::Drainage before = hog::globe::drain(ground, level, radius, 0.0);
    const std::vector<float> after =
        hog::globe::erode(ground, level, radius, 0.0, {.rate = 1.0e-3, .steps = 5});
    double cut_most = 0.0;
    double drains_most = 0.0;
    for (std::size_t cell = 0; cell < ground.size(); ++cell) {
        CHECK(after[cell] <= ground[cell]);
        if (ground[cell] <= 0.0F) {
            CHECK(after[cell] == ground[cell]);
        }
        if (ground[cell] - after[cell] > cut_most) {
            cut_most = ground[cell] - after[cell];
            drains_most = before.catchment[cell];
        }
    }
    CHECK(cut_most > 0.0);
    // The deepest cut is under well more than an average cell's share.
    CHECK(drains_most > 10.0 * before.catchment[0] / 4.0);
    CHECK(hog::globe::erode(ground, level, radius, 0.0, {.rate = 1.0e-3, .steps = 5}) == after);
}
