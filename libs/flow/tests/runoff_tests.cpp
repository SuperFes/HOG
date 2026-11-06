#include <hog/flow/runoff.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <functional>

using Catch::Approx;
using hog::flow::Runoff;

namespace {

constexpr int side = 31;
constexpr int middle_of = side / 2;
constexpr int cliff = side / 2;
constexpr double cell = 2.0;

std::size_t at(int x, int y) {
    return (static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x);
}

// A square of columns of ground by `height(x, y)`, all under `rain`, letting water go at the
// edge columns `out` picks.
Runoff square(const std::function<double(int, int)>& height, double rain,
              const std::function<bool(int, int)>& out, Runoff::Law law = {.cell = cell}) {
    Runoff runoff{static_cast<std::size_t>(side * side), law};
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            Runoff::Node& node = runoff[at(x, y)];
            node.ground = height(x, y);
            node.rain = rain;
            node.outlet = out(x, y);
            node.next = {x + 1 < side ? static_cast<int>(at(x + 1, y)) : Runoff::none,
                         y + 1 < side ? static_cast<int>(at(x, y + 1)) : Runoff::none,
                         x > 0 ? static_cast<int>(at(x - 1, y)) : Runoff::none,
                         y > 0 ? static_cast<int>(at(x, y - 1)) : Runoff::none};
        }
    }
    return runoff;
}

// A bowl whose rim is lowest on its +x side, where the edge lets water go.
double bowl(int x, int y) {
    const auto dx = static_cast<double>(x - middle_of);
    const auto dy = static_cast<double>(y - middle_of);
    return 100.0 + (0.02 * ((dx * dx) + (dy * dy))) - (0.05 * dx);
}

bool east_edge(int x, int /*y*/) { return x == side - 1; }

} // namespace

TEST_CASE("a hollow under enough rain fills to its rim and spills over", "[flow][runoff]") {
    Runoff runoff = square(bowl, 2.0, east_edge, {.cell = cell, .evaporation = 0.5});
    runoff.settle();
    const std::size_t middle = at(side / 2, side / 2);
    CHECK(runoff.standing(middle));
    CHECK(runoff.depth(middle) > 1.0);
    // Level all across, as high as the lowest way out over the edge.
    double rim = 1e9;
    for (int y = 0; y < side; ++y) {
        rim = std::min(rim, bowl(side - 1, y));
    }
    CHECK(runoff.surface(middle) == Approx(rim).margin(0.3));
    // What spills runs on to the edge: all the rain less what soaked in or rose off the lake.
    double out = 0.0;
    for (int y = 0; y < side; ++y) {
        out += runoff.discharge(at(side - 1, y));
    }
    CHECK(out > 0.0);
    CHECK(out < 2.0 * cell * cell * side * side);
}

TEST_CASE("a hollow under little rain fills only as far as the air takes back what comes",
          "[flow][runoff]") {
    Runoff runoff = square(bowl, 0.2, east_edge, {.cell = cell, .evaporation = 2.0});
    runoff.settle();
    const std::size_t middle = at(side / 2, side / 2);
    CHECK(runoff.standing(middle));
    // Lower than the rim, and nothing spills: the edge lets go only what falls on it.
    CHECK(runoff.surface(middle) < bowl(side - 1, side / 2) - 0.5);
    double out = 0.0;
    for (int y = 0; y < side; ++y) {
        out += runoff.discharge(at(side - 1, y));
    }
    CHECK(out == Approx(0.2 * 0.7 * cell * cell * side));
    // The lake's surface is what all the rain over the bowl can keep wet.
    std::size_t wet = 0;
    for (std::size_t n = 0; n < runoff.size(); ++n) {
        wet += runoff.standing(n) ? 1U : 0U;
    }
    CHECK(wet > 0);
    CHECK(static_cast<double>(wet) * 2.0 < 0.2 * side * side);
}

TEST_CASE("without rain the ground stays dry", "[flow][runoff]") {
    Runoff runoff = square(bowl, 0.0, east_edge);
    runoff.settle();
    for (std::size_t n = 0; n < runoff.size(); ++n) {
        CHECK(runoff.depth(n) == 0.0);
    }
}

TEST_CASE("a valley carries a stream, deeper downstream", "[flow][runoff]") {
    // Falling along +x, its floor down the middle, with a river coming in at its head.
    const auto valley = [](int x, int y) {
        return 200.0 - (1.0 * x) + (0.5 * std::abs(y - (side / 2)));
    };
    Runoff runoff = square(valley, 1.0, east_edge, {.cell = cell, .shallowest = 0.001});
    runoff[at(0, side / 2)].inflow = 1.0e7;
    runoff.settle();
    const std::size_t up = at(5, side / 2);
    const std::size_t down = at(side - 3, side / 2);
    CHECK_FALSE(runoff.standing(down));
    CHECK(runoff.depth(up) > 0.0);
    CHECK(runoff.depth(down) > runoff.depth(up));
    CHECK(runoff.discharge(down) > runoff.discharge(up));
    // Off the floor, the slopes only shed it.
    CHECK(runoff.depth(at(side - 3, 2)) < runoff.depth(down));
}

TEST_CASE("water from beyond runs through, and a high hollow spills over a drop",
          "[flow][runoff]") {
    // A shelf high on the -x side with a hollow in it, ending in a cliff down to a plain.
    const auto shelf = [](int x, int y) {
        if (x >= cliff) {
            return 10.0 - (0.01 * x);
        }
        constexpr int hollow_x = side / 4;
        const auto dx = static_cast<double>(x - hollow_x);
        const auto dy = static_cast<double>(y - middle_of);
        return 60.0 + (0.05 * ((dx * dx) + (dy * dy)));
    };
    Runoff runoff = square(shelf, 0.0, east_edge, {.cell = cell, .evaporation = 0.5});
    runoff[at(0, side / 2)].inflow = 5.0e5;
    runoff.settle();
    CHECK(runoff.standing(at(side / 4, middle_of)));
    // It spills over the cliff's top and on across the plain below.
    std::size_t lip = at(cliff - 1, middle_of);
    for (int y = 0; y < side; ++y) {
        if (runoff.discharge(at(cliff - 1, y)) > runoff.discharge(lip)) {
            lip = at(cliff - 1, y);
        }
    }
    CHECK(runoff.discharge(lip) > 1.0e5);
    double out = 0.0;
    for (int y = 0; y < side; ++y) {
        out += runoff.discharge(at(side - 1, y));
    }
    CHECK(out == Approx(runoff.discharge(lip)).epsilon(0.01));
}
