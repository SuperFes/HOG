#include <hog/server/ground_wetting.hpp>

#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <tuple>

namespace {

using hog::DVec3;
using hog::server::GroundWetting;

const hog::globe::Generator& earth() {
    static const hog::globe::Generator generator{hog::globe::Planet{}};
    return generator;
}

} // namespace

TEST_CASE("rain wets the ground, snow doesn't, and dry air dries it", "[server][wetting]") {
    const auto rain = GroundWetting::sky_of({.falling = 4.0F, .snow = 0.1F, .humidity = 0.9F});
    CHECK(rain.rain == 4.0);
    CHECK(rain.dryness == 0.0);
    const auto snow = GroundWetting::sky_of({.falling = 4.0F, .snow = 0.9F, .humidity = 0.9F});
    CHECK(snow.rain == 0.0);
    const auto clear = GroundWetting::sky_of({.falling = 0.0F, .snow = 0.0F, .humidity = 0.25F});
    CHECK(clear.rain == 0.0);
    CHECK(clear.dryness == 0.75);
}

TEST_CASE("the ground around a place is taken once, a square of columns, each above its ground",
          "[server][wetting]") {
    const DVec3 up = glm::normalize(DVec3{0.3, 0.4, 0.86});
    const hog::flow::Sky sky{.rain = 3.0};
    const std::vector<GroundWetting::Place> places{{.up = up, .sky = sky}, {.up = up, .sky = {}}};
    const auto columns = GroundWetting::around(earth(), places);
    const int side = (2 * GroundWetting::reach) + 1;
    CHECK(columns.size() <= static_cast<std::size_t>(side * side));
    CHECK(columns.size() > static_cast<std::size_t>(side * side) * 9 / 10);
    std::set<std::tuple<int, std::uint32_t, std::uint32_t>> seen;
    for (const auto& one : columns) {
        CHECK(seen.emplace(one.column.face, one.column.i, one.column.j).second);
        CHECK(one.sky.rain == 3.0);
        CHECK(one.column.layer > 0);
    }
}
