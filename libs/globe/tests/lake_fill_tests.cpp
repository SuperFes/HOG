#include "../src/lake_fill.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <utility>

using hog::globe::beside;
using hog::globe::fill_pool;
using hog::globe::middle_of;
using hog::globe::Pool;
using hog::globe::Spot;

namespace {

// A round hollow, its floor at 0 in the middle and rising a metre a cell squared over ten,
// with a notch cut through its rim eastward down to `notch`.
double bowl(double x, double y, double notch) {
    const double rise = ((x * x) + (y * y)) / 10.0;
    if (x > 0 && std::abs(y) <= 1) {
        // The notch's floor falls away past the rim.
        return std::min(rise, notch - (0.01 * (x - 15)));
    }
    return rise;
}

} // namespace

TEST_CASE("water fills a hollow up to the notch it runs out of, and stands level there",
          "[globe][lake]") {
    constexpr double notch = 22.0;
    const Pool pool = fill_pool(40, [](double x, double y) { return bowl(x, y, notch); }, 1e9, 0.0);
    // The notch's lowest point on the way out is where its floor meets the rising bowl.
    CHECK(pool.level <= notch + 0.1);
    CHECK(pool.level >= notch - 0.5);
    // Water over every cell under the level inside the rim, none past the notch.
    for (int y = -40; y <= 40; ++y) {
        for (int x = -40; x <= 40; ++x) {
            const double ground = bowl(x, y, notch);
            const bool inside = (x * x) + (y * y) < 15 * 15;
            CAPTURE(x, y, ground);
            if (inside && ground < pool.level) {
                CHECK(pool.at(x, y) == Pool::Cell::Water);
            }
            if (!inside) {
                CHECK(pool.at(x, y) != Pool::Cell::Water);
            }
        }
    }
}

TEST_CASE("water that hasn't enough to brim stands at what it has, under its rim",
          "[globe][lake]") {
    const Pool pool = fill_pool(40, [](double x, double y) { return bowl(x, y, 22.0); }, 5.0, 0.0);
    CHECK(pool.level == 5.0);
    CHECK(pool.at(0, 0) == Pool::Cell::Water);
    // Seven cells out the bowl stands at 4.9, eight out at 6.4: the shore.
    CHECK(pool.at(7, 0) == Pool::Cell::Water);
    CHECK(pool.at(8, 0) == Pool::Cell::Shore);
    CHECK(pool.at(9, 0) == Pool::Cell::Dry);
}

TEST_CASE("the slope past the rim water runs out over stays dry", "[globe][lake]") {
    // A hollow whose east side gives way to ground falling to the grid's edge, lowest where it
    // meets the hollow ten cells east, at 10.
    const auto ground = [](double x, double y) {
        const double near = ((x * x) + (y * y)) / 10.0;
        return x > 0 ? std::min(near, 20.0 - x) : near;
    };
    const Pool pool = fill_pool(40, ground, 1e9, 0.0);
    CHECK(pool.level == 10.0);
    CHECK(pool.at(0, 0) == Pool::Cell::Water);
    CHECK(pool.at(9, 0) == Pool::Cell::Water);
    CHECK(pool.at(15, 0) == Pool::Cell::Dry);
    CHECK(pool.at(30, 0) == Pool::Cell::Dry);
}

TEST_CASE("water rising past its own reach fills on out, never cut off at a grid's edge",
          "[globe][lake]") {
    // A bowl wider than the water's own reach of ten cells.
    const Pool pool =
        fill_pool(10, [](double x, double y) { return bowl(x, y, 1e9); }, 30.0, 0.0, 40);
    CHECK(pool.level == 30.0);
    CHECK_FALSE(pool.outlet.has_value());
    // Out to where the bowl stands at the level, past the ten cells.
    CHECK(pool.at(16, 0) == Pool::Cell::Water);
    CHECK(pool.at(0, -17) == Pool::Cell::Water);
    CHECK(pool.at(18, 0) != Pool::Cell::Water);
}

TEST_CASE("water that brims and runs on out of its reach marks the rim it ran out over",
          "[globe][lake]") {
    // As above, the hollow's east side giving way to ground falling away ten cells east.
    std::size_t read = 0;
    const auto ground = [&](double x, double y) {
        ++read;
        const double near = ((x * x) + (y * y)) / 10.0;
        return x > 0 ? std::min(near, 20.0 - x) : near;
    };
    const Pool pool = fill_pool(15, ground, 1e9, 0.0, 2'000);
    CHECK(pool.level == 10.0);
    REQUIRE(pool.outlet.has_value());
    const auto [outlet_x, outlet_y] = pool.outlet.value_or(std::pair{0.0, 0.0});
    CHECK(outlet_x == 10.0);
    // On the rim, which stands at 10 all along ten cells east.
    CHECK(ground(outlet_x, outlet_y) == 10.0);
    CHECK(pool.at(9, 0) == Pool::Cell::Water);
    CHECK(pool.at(12, 0) == Pool::Cell::Dry);
    // Stopping there, not running on down to the grid's far edge.
    CHECK(read < 2'000);
    // Water held short of its rim runs out nowhere.
    CHECK_FALSE(fill_pool(15, ground, 5.0, 0.0, 2'000).outlet.has_value());
}

TEST_CASE("water running over into a further closed hollow fills it too", "[globe][lake]") {
    // Two hollows side by side, the far one deeper, closed by ground rising to the grid's edge:
    // water poured in the near one runs over into it, and both fill as one.
    const auto ground = [](double x, double y) {
        const double near = ((x * x) + (y * y)) / 10.0;
        const double far = ((((x - 20) * (x - 20)) + (y * y)) / 10.0) - 5.0;
        return std::min(near, far);
    };
    const Pool pool = fill_pool(40, ground, 20.0, 0.0);
    CHECK(pool.level == 20.0);
    CHECK(pool.at(0, 0) == Pool::Cell::Water);
    CHECK(pool.at(20, 0) == Pool::Cell::Water);
    CHECK(pool.at(9, 0) == Pool::Cell::Water);
}

TEST_CASE("water poured on a hollow's side runs down and fills it from its bottom",
          "[globe][lake]") {
    const Pool pool =
        fill_pool(40, [](double x, double y) { return bowl(x - 6, y + 4, 22.0); }, 5.0, 0.0);
    CHECK(pool.level == 5.0);
    CHECK(pool.at(6, -4) == Pool::Cell::Water);
    CHECK(pool.at(4, -2) == Pool::Cell::Water);
}

TEST_CASE("a fill reads only the ground near the water", "[globe][lake]") {
    std::size_t read = 0;
    const Pool pool = fill_pool(
        200,
        [&](double x, double y) {
            ++read;
            return bowl(x, y, 22.0);
        },
        5.0, 0.0);
    CHECK(pool.level == 5.0);
    CHECK(read < 400);
}

TEST_CASE("a lake's shore runs on over ground its cells' middles stand over but dips under",
          "[globe][lake]") {
    // A hollow filled to 5, with a shelf east of it whose cells' middles stand a little over the
    // water but which dips under it here and there all the way out.
    const auto ground = [](double x, double y) {
        const double hollow = ((x * x) + (y * y)) / 10.0;
        if (x > 7 && x < 20 && std::abs(y) < 3) {
            return std::min(hollow, 5.2);
        }
        return hollow;
    };
    const Pool pool = fill_pool(40, ground, 5.0, 1.0);
    CHECK(pool.level == 5.0);
    for (int x = 8; x < 20; ++x) {
        CAPTURE(x);
        CHECK(pool.at(x, 0) == Pool::Cell::Shore);
    }
    // Higher ground beyond stays dry.
    CHECK(pool.at(25, 0) == Pool::Cell::Dry);
    // Without the room, the shore stops at the first cell.
    const Pool tight = fill_pool(40, ground, 5.0, 0.0);
    CHECK(tight.at(8, 0) == Pool::Cell::Shore);
    CHECK(tight.at(12, 0) == Pool::Cell::Dry);
}

TEST_CASE("water split about its edge runs out through a gap in its rim narrower than a cell",
          "[globe][lake]") {
    // A hollow rimmed at 10 ten cells out, falling away past it, with a gap a third of a cell
    // wide cut through the rim between two rows of cells, its floor falling away from 4.
    const auto ground = [](double x, double y) {
        const double r = std::hypot(x, y);
        const double hollow = r < 10.0 ? r * r / 10.0 : 10.0 - (r - 10.0);
        if (x > 5.0 && std::abs(y - 0.5) < 0.15) {
            return std::min(hollow, 4.0 - (0.1 * (x - 5.0)));
        }
        return hollow;
    };
    // Whole cells never see the gap, and run out over the rim.
    CHECK(fill_pool(15, ground, 1e9, 0.0, 2'000).level > 9.0);
    // Split, it runs out where the gap meets the hollow, about 3.9 up.
    const Pool pool = fill_pool(15, ground, 1e9, 0.0, 2'000, 8);
    CHECK(pool.level > 3.7);
    CHECK(pool.level < 4.0);
    REQUIRE(pool.outlet.has_value());
    const auto [outlet_x, outlet_y] = pool.outlet.value_or(std::pair{0.0, 0.0});
    CHECK(std::abs(outlet_y - 0.5) < 0.2);
    CHECK(outlet_x > 5.0);
    CHECK(outlet_x < 7.0);
}

TEST_CASE("water split about its edge leaves no ground under its level dry beside it",
          "[globe][lake]") {
    // A hollow rippled finer than its cells, filled to 6.
    const auto ground = [](double x, double y) {
        return (((x * x) + (y * y)) / 10.0) + (1.5 * std::sin(2.7 * x) * std::sin(2.3 * y));
    };
    constexpr int fine = 8;
    // Dry parts under the level beside water or shore: walls of water.
    const auto walls = [&](const Pool& pool) {
        int found = 0;
        for (int y = -15 * fine; y <= 15 * fine; ++y) {
            for (int x = -15 * fine; x <= 15 * fine; ++x) {
                const auto [at_x, at_y] = middle_of(Spot{.x = x, .y = y, .part = true}, fine);
                if (pool.at(at_x, at_y) != Pool::Cell::Dry || ground(at_x, at_y) >= pool.level) {
                    continue;
                }
                for (const auto& [next_x, next_y] : beside(x, y)) {
                    const auto [to_x, to_y] =
                        middle_of(Spot{.x = next_x, .y = next_y, .part = true}, fine);
                    if (pool.at(to_x, to_y) != Pool::Cell::Dry) {
                        ++found;
                        break;
                    }
                }
            }
        }
        return found;
    };
    const Pool coarse = fill_pool(40, ground, 6.0, 0.0);
    const Pool split = fill_pool(40, ground, 6.0, 0.0, 0, fine);
    CHECK(split.level == 6.0);
    CHECK(walls(coarse) > 0);
    CHECK(walls(split) == 0);
}
