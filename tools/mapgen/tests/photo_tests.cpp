#include <hog/mapgen/photo.hpp>

#include <catch2/catch_test_macros.hpp>

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
