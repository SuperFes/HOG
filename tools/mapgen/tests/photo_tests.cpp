#include <hog/mapgen/photo.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using hog::DVec2;
using hog::DVec3;
using hog::mapgen::Lighting;
using hog::mapgen::LinearRgb;
using hog::mapgen::true_colour;
using hog::planet::Biome;

namespace {

double brightness(LinearRgb colour) {
    return (0.2126 * colour.r) + (0.7152 * colour.g) + (0.0722 * colour.b);
}

} // namespace

TEST_CASE("the sea darkens with depth and whitens under ice", "[mapgen][photo]") {
    const LinearRgb shallow = true_colour({.biome = Biome::Ocean, .depth = 5.0});
    const LinearRgb deep = true_colour({.biome = Biome::Ocean, .depth = 3'000.0});
    CHECK(brightness(deep) < 0.5 * brightness(shallow));
    CHECK(deep.b > deep.r);
    const LinearRgb iced = true_colour({.biome = Biome::Ocean, .depth = 3'000.0, .ice = 1.0});
    CHECK(brightness(iced) > 0.6);
    CHECK(brightness(true_colour({.biome = Biome::Ocean, .depth = 3'000.0, .ice = 0.5})) <
          brightness(iced));
}

TEST_CASE("plants hide the bare earth as they grow", "[mapgen][photo]") {
    const LinearRgb bare = true_colour({.biome = Biome::TemperateForest, .plants = 0.0});
    const LinearRgb grown = true_colour({.biome = Biome::TemperateForest, .plants = 1.0});
    CHECK(brightness(grown) < brightness(bare));
    CHECK(grown.g > grown.r);
    CHECK(bare.r > bare.g);
    // A desert's few plants barely change it.
    const LinearRgb sand = true_colour({.biome = Biome::Desert, .plants = 0.0});
    const LinearRgb scrub = true_colour({.biome = Biome::Desert, .plants = 1.0});
    CHECK(brightness(sand) - brightness(scrub) < 0.2 * brightness(sand));
}

TEST_CASE("wet ground is darker than dry", "[mapgen][photo]") {
    const LinearRgb dry = true_colour({.biome = Biome::Desert, .plants = 0.0, .soil = 0.0});
    const LinearRgb wet = true_colour({.biome = Biome::Desert, .plants = 0.0, .soil = 1.0});
    CHECK(brightness(wet) < 0.8 * brightness(dry));
}

TEST_CASE("snow whitens the ground, less so under trees", "[mapgen][photo]") {
    const LinearRgb field = true_colour({.biome = Biome::Grassland});
    const LinearRgb dusted = true_colour({.biome = Biome::Grassland, .snow = 5.0});
    const LinearRgb buried = true_colour({.biome = Biome::Grassland, .snow = 500.0});
    CHECK(brightness(dusted) > brightness(field));
    CHECK(brightness(buried) > brightness(dusted));
    CHECK(brightness(buried) > 0.7);
    const LinearRgb forest = true_colour({.biome = Biome::BorealForest, .snow = 500.0});
    CHECK(brightness(forest) < 0.8 * brightness(buried));
    // Packed into a glacier, it greys and blues.
    const LinearRgb glacier = true_colour({.biome = Biome::Grassland, .snow = 3'000.0});
    CHECK(brightness(glacier) < brightness(buried));
    CHECK(glacier.b - glacier.r > buried.b - buried.r);
}

TEST_CASE("rivers narrow in a drought and muddy in flood", "[mapgen][photo]") {
    const LinearRgb ground = true_colour({.biome = Biome::Grassland});
    const LinearRgb river = true_colour({.biome = Biome::Grassland, .river = 1.0});
    CHECK(brightness(river) < brightness(ground));
    CHECK(river.b > river.r);
    const LinearRgb low = true_colour({.biome = Biome::Grassland, .river = 1.0, .flow = 0.0});
    CHECK(brightness(low) > brightness(river));
    CHECK(brightness(low) < brightness(ground));
    const LinearRgb flood = true_colour({.biome = Biome::Grassland, .river = 1.0, .flow = 6.0});
    CHECK(flood.r > flood.b);
}

TEST_CASE("fire chars the ground and glows", "[mapgen][photo]") {
    const LinearRgb forest = true_colour({.biome = Biome::TemperateForest});
    const LinearRgb burning = true_colour({.biome = Biome::TemperateForest, .fire = 1.0});
    CHECK(burning.r > burning.g);
    CHECK(burning.r > 2.0 * burning.b);
    CHECK(true_colour({.biome = Biome::TemperateForest, .fire = 0.2}).r > forest.r);
}

TEST_CASE("lakes are dark water that freezes white", "[mapgen][photo]") {
    const LinearRgb lake = true_colour({.biome = Biome::Lake});
    CHECK(brightness(lake) < brightness(true_colour({.biome = Biome::Grassland})));
    CHECK(lake == true_colour({.biome = Biome::Lake, .plants = 0.0, .soil = 1.0}));
    CHECK(brightness(true_colour({.biome = Biome::Lake, .snow = 500.0})) > 0.7);
}

TEST_CASE("the sun stands where the lighting puts it", "[mapgen][photo]") {
    const hog::planet::TangentFrame frame{};
    const DVec3 sun = hog::mapgen::sun_towards(Lighting{}, frame);
    CHECK(glm::length(sun) == Catch::Approx(1.0));
    CHECK(sun.z == Catch::Approx(std::sin(glm::radians(35.0))));
    // From the north-west.
    CHECK(sun.x < 0.0);
    CHECK(sun.y == Catch::Approx(-sun.x));
    const DVec3 overhead = hog::mapgen::sun_towards(Lighting{.sun_elevation = 90.0}, frame);
    CHECK(overhead.z == Catch::Approx(1.0));
}

TEST_CASE("a surface faces away from the ground falling under it", "[mapgen][photo]") {
    using hog::mapgen::surface_normal;
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 flat =
        surface_normal({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, up);
    CHECK(flat.z == Catch::Approx(1.0));
    // Falling away eastwards, so it faces east.
    const DVec3 slope =
        surface_normal({-1.0, 0.0, 1.0}, {1.0, 0.0, -1.0}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, up);
    CHECK(slope.x == Catch::Approx(std::sqrt(0.5)));
    CHECK(slope.z == Catch::Approx(std::sqrt(0.5)));
    // The image's rows may run either way.
    CHECK(surface_normal({-1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, up)
              .z == Catch::Approx(1.0));
    CHECK(surface_normal({}, {}, {}, {}, up) == up);
}

TEST_CASE("slopes facing the sun are lit brighter and shadows by the sky alone",
          "[mapgen][photo]") {
    using hog::mapgen::light;
    const Lighting lighting{};
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 sun = hog::mapgen::sun_towards(lighting, {});
    const double flat = light(up, up, sun, lighting.sky, 1.0);
    CHECK(flat / hog::mapgen::exposure(lighting) == Catch::Approx(1.0));
    CHECK(light(glm::normalize(up + sun), up, sun, lighting.sky, 1.0) > flat);
    const DVec3 away = glm::normalize(up - (2.0 * sun));
    CHECK(light(away, up, sun, lighting.sky, 1.0) < 0.5 * flat);
    const double shaded = light(up, up, sun, lighting.sky, 0.0);
    CHECK(shaded == Catch::Approx(lighting.sky).margin(0.02));
    const double hazy = light(up, up, sun, lighting.sky, 0.5);
    CHECK(hazy > shaded);
    CHECK(hazy < flat);
}

TEST_CASE("higher ground towards the sun casts a shadow", "[mapgen][photo]") {
    using hog::mapgen::in_shadow;
    constexpr std::array heights{0.0, 0.0, 0.0, 100.0, 0.0};
    const hog::mapgen::HeightField field{.width = 5, .height = 1, .heights = heights};
    CHECK(in_shadow(field, 0, 0, DVec2{1.0, 0.0}, 10.0, 8));
    // A sun high enough clears it.
    CHECK_FALSE(in_shadow(field, 0, 0, DVec2{1.0, 0.0}, 50.0, 8));
    // Too far to reach.
    CHECK_FALSE(in_shadow(field, 0, 0, DVec2{1.0, 0.0}, 10.0, 2));
    // Looking away from it, off the edge.
    CHECK_FALSE(in_shadow(field, 2, 0, DVec2{-1.0, 0.0}, 0.0, 8));
    CHECK_FALSE(in_shadow(field, 3, 0, DVec2{1.0, 0.0}, 0.0, 8));
}

TEST_CASE("cloud gathers over the share of the ground its cover gives", "[mapgen][photo]") {
    using hog::mapgen::Cloud;
    using hog::mapgen::cloud_noise;
    using hog::mapgen::cloud_over;
    CHECK(cloud_over(0.0, 1.0) == Cloud{});
    CHECK(cloud_over(1.0, -1.0) == Cloud{.shape = 1.0, .depth = 1.0});
    CHECK(cloud_over(1.0, -1.0).opacity() == 1.0);
    CHECK(cloud_over(0.5, 0.3).shape > cloud_over(0.5, -0.3).shape);
    CHECK(cloud_over(0.8, 0.0).shape > cloud_over(0.2, 0.0).shape);
    for (const double cover : {0.2, 0.5, 0.8}) {
        double total = 0.0;
        constexpr int samples = 4'000;
        for (int sample = 0; sample < samples; ++sample) {
            const DVec3 metres{sample * 7'919.0, sample * 3'217.0, sample * -5'003.0};
            total += cloud_over(cover, cloud_noise(9, metres, 12'000.0)).shape;
        }
        CHECK(total / samples == Catch::Approx(cover).margin(0.07));
    }
}

TEST_CASE("cloud is thin at its edges and thick deep in", "[mapgen][photo]") {
    using hog::mapgen::cloud_over;
    const auto edge = cloud_over(0.5, 0.0);
    const auto deep = cloud_over(0.5, 0.4);
    CHECK(edge.depth < deep.depth);
    CHECK(edge.opacity() < 0.5 * deep.opacity());
    CHECK(deep.opacity() > 0.9);
    CHECK(brightness(hog::mapgen::cloud_colour(deep, 0.0)) >
          brightness(hog::mapgen::cloud_colour(edge, 0.0)));
}

TEST_CASE("cloud noise follows its seed", "[mapgen][photo]") {
    using hog::mapgen::cloud_noise;
    const DVec3 metres{12'345.0, -6'789.0, 1'011.0};
    CHECK(cloud_noise(1, metres, 10'000.0) == cloud_noise(1, metres, 10'000.0));
    CHECK(cloud_noise(1, metres, 10'000.0) != cloud_noise(2, metres, 10'000.0));
    CHECK(std::abs(cloud_noise(1, metres, 10'000.0)) <= 1.0);
}

TEST_CASE("storm tops are whiter than other cloud", "[mapgen][photo]") {
    const hog::mapgen::Cloud cloud{.shape = 1.0, .depth = 0.5};
    CHECK(brightness(hog::mapgen::cloud_colour(cloud, 1.0)) >
          brightness(hog::mapgen::cloud_colour(cloud, 0.0)));
    CHECK(brightness(hog::mapgen::cloud_colour(cloud, 0.0)) > 0.5);
}

TEST_CASE("night falls where the sun has set", "[mapgen][photo]") {
    using hog::mapgen::light;
    const Lighting lighting{.real_sun = true};
    const DVec3 up{0.0, 0.0, 1.0};
    // Exposed for the sun overhead.
    CHECK(light(up, up, up, lighting.sky, 1.0) / hog::mapgen::exposure(lighting) ==
          Catch::Approx(1.0));
    const DVec3 set = glm::normalize(DVec3{1.0, 0.0, -0.6});
    const double night = light(up, up, set, lighting.sky, 1.0);
    CHECK(night > 0.0);
    CHECK(night < 0.02);
    // A slope facing a sun that has set still lies in the planet's shadow.
    CHECK(light(glm::normalize(DVec3{1.0, 0.0, 0.2}), up, set, lighting.sky, 1.0) == night);
}

TEST_CASE("the sun glints off water below it", "[mapgen][photo]") {
    using hog::mapgen::sea_glint;
    const DVec3 up{0.0, 0.0, 1.0};
    CHECK(sea_glint(up, up) == Catch::Approx(1.0));
    const DVec3 high = hog::mapgen::sun_towards(Lighting{.sun_elevation = 75.0}, {});
    const DVec3 low = hog::mapgen::sun_towards(Lighting{}, {});
    CHECK(sea_glint(up, high) < 1.0);
    CHECK(sea_glint(up, high) > sea_glint(up, low));
    CHECK(sea_glint(up, low) < 0.1);
    CHECK(sea_glint(up, -up) == 0.0);
}

TEST_CASE("open water is sea free of ice and lakes free of snow", "[mapgen][photo]") {
    using hog::mapgen::open_water;
    CHECK(open_water({.biome = Biome::Ocean}) == 1.0);
    CHECK(open_water({.biome = Biome::Ocean, .ice = 0.25}) == 0.75);
    CHECK(open_water({.biome = Biome::Lake}) == 1.0);
    CHECK(open_water({.biome = Biome::Lake, .snow = 100.0}) < 0.25);
    CHECK(open_water({.biome = Biome::Grassland, .river = 1.0}) == 0.0);
}

TEST_CASE("clouds shade the ground away from the sun", "[mapgen][photo]") {
    using hog::mapgen::towards_clouds;
    constexpr double radius = 100'000.0;
    const DVec3 up{0.0, 0.0, 1.0};
    CHECK(glm::length(towards_clouds(up, up, 3'000.0, radius) - up) < 1e-12);
    // With the sun 45° up in the east, the cloud shading a point lies its height to the east.
    const DVec3 east_sun = glm::normalize(DVec3{1.0, 0.0, 1.0});
    const DVec3 above = towards_clouds(up, east_sun, 3'000.0, radius);
    CHECK(above.x * radius == Catch::Approx(3'000.0).epsilon(0.05));
    CHECK(above.y == 0.0);
    // Held near with the sun on the horizon.
    const DVec3 low = towards_clouds(up, DVec3{1.0, 0.0, 0.0}, 3'000.0, radius);
    CHECK(low.x * radius == Catch::Approx(30'000.0).epsilon(0.05));
}
