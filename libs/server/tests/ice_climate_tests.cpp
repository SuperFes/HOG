#include <hog/server/ice_climate.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hog::server;

TEST_CASE("the flow's ice balance follows the weather's climate at the ground's height",
          "[server][ice]") {
    const hog::globe::Planet planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    const auto balance = ice_balance(planet, [](const hog::globe::Cell& column) {
        // Cold and snowy on the odd columns, warm on the even.
        return column.i % 2 == 1
                   ? hog::weather::Climate{.surface = 262.0, .swing = 8.0, .precipitation = 0.15}
                   : hog::weather::Climate{.surface = 295.0, .swing = 8.0, .precipitation = 0.15};
    });
    const hog::globe::Cell cold{.level = 10, .face = 0, .i = 1, .j = 0};
    const hog::globe::Cell warm{.level = 10, .face = 0, .i = 2, .j = 0};
    CHECK(balance(cold, planet.radius) > 0.0);
    CHECK(balance(warm, planet.radius) < 0.0);
    // Higher is better for snow than lower.
    CHECK(balance(warm, planet.radius + 6'000.0) > balance(warm, planet.radius));
}
