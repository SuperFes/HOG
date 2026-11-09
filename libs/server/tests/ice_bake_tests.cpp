#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/server/ice_bake.hpp>
#include <hog/weather/field.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace hog::server;
using namespace hog::globe;

namespace {

const Generator& earth() {
    static const Generator generator{Planet{}};
    return generator;
}

// The cell under the highest ground of a coarse sweep of the sphere.
Cell highest() {
    Cell best{
        .level = static_cast<std::uint8_t>(earth().layers().level), .face = 0, .i = 0, .j = 0};
    double top = 0.0;
    constexpr int points = 40'000;
    for (int k = 0; k < points; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / points);
        const double r = std::sqrt(1.0 - (z * z));
        const double a = 2.399963 * k;
        const hog::DVec3 d{r * std::cos(a), r * std::sin(a), z};
        if (const double ground = earth().column(d, 200.0).ground(); ground > top) {
            top = ground;
            best = cell_under(d, earth().layers().level);
        }
    }
    return best;
}

// A climate freezing halfway up the highest mountain, so whatever the world, its heights hold ice.
hog::weather::Climate climate_of(const Cell& /*column*/) {
    static const double freezing_level = [] {
        const Planet& planet = earth().planet();
        return 0.5 * (earth().column(center(highest()), 1.0).ground() - planet.body.radius);
    }();
    return {.surface = 273.15 + (hog::weather::lapse_rate(earth().planet()) * freezing_level),
            .swing = 12.0,
            .precipitation = 0.15};
}

} // namespace

TEST_CASE("an ice map reads between its nodes and is bare outside", "[server][ice]") {
    IceMap map{.corner = {}, .stride = 4, .side = 2, .ice = {10.0, 20.0, 30.0, 40.0}};
    CHECK(map.columns() == 8);
    CHECK(map.at(8, 0) == 0.0);
    // Past the outer nodes' middles it holds the edge value, and between them it rises.
    CHECK(map.at(0, 0) == Catch::Approx(10.0));
    CHECK(map.at(7, 7) == Catch::Approx(40.0));
    CHECK(map.at(5, 1) > map.at(1, 1));
    CHECK(map.at(1, 5) > map.at(1, 1));
    CHECK(map.at(3, 3) > map.at(2, 2));
}

TEST_CASE("a mountain's snow settles to ice on its heights and flows off its rim",
          "[server][ice]") {
    const Cell peak = highest();
    constexpr std::uint32_t side = 40;
    constexpr std::uint32_t stride = 16;
    const Cell corner{.level = peak.level,
                      .face = peak.face,
                      .i = peak.i - (side * stride / 2),
                      .j = peak.j - (side * stride / 2)};
    const IceMap map = bake_ice_map(earth(), corner, side, stride, climate_of, 200);
    double most = 0.0;
    double rim = 0.0;
    for (std::uint32_t b = 0; b < side; ++b) {
        for (std::uint32_t a = 0; a < side; ++a) {
            const double ice = map.ice[(static_cast<std::size_t>(b) * side) + a];
            most = std::max(most, ice);
            if (a == 0 || b == 0 || a + 1 == side || b + 1 == side) {
                rim = std::max(rim, ice);
            }
        }
    }
    CHECK(most > 5.0);
    CHECK(rim == 0.0);
    // Read back between the nodes, it never passes the thickest.
    CHECK(map.at((side / 2 * stride) + (stride / 2), (side / 2 * stride) + (stride / 2)) <= most);
    if (const char* dir = std::getenv("HOG_LOOK_DIR")) {
        std::ofstream out{std::string{dir} + "/ice_map.pgm", std::ios::binary};
        out << "P5\n" << side << " " << side << "\n255\n";
        for (const double ice : map.ice) {
            out.put(static_cast<char>(std::min(255.0, ice * 2.0)));
        }
    }
}
