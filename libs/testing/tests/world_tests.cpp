#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

TEST_CASE("the small world forms quickly, with sea and land", "[testing][world]") {
    const auto started = std::chrono::steady_clock::now();
    const hog::globe::Generator& world = hog::testing::small_world();
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;
    WARN("formed in " << took.count() << " s");
    CHECK(world.planet().body.radius == 12'000.0);
    REQUIRE(world.sea_level().has_value());
    CHECK(world.shape().lowest < 0.0);
    CHECK(world.shape().highest > 100.0);
    // The same planet every time it's asked for.
    CHECK(hog::globe::world_id(hog::testing::small_planet()) ==
          hog::globe::world_id(world.planet()));
}
