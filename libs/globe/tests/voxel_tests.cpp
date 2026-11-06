#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using hog::globe::Material;
using hog::globe::Voxel;

TEST_CASE("voxels hold amounts and temperature in fixed steps", "[globe][voxel]") {
    CHECK(hog::globe::amount_of(0.0) == 0);
    CHECK(hog::globe::amount_of(1.0) == Voxel::full);
    CHECK(hog::globe::amount_of(2.0) == Voxel::full);
    CHECK(hog::globe::amount_of(-1.0) == 0);
    CHECK(hog::globe::amount_of(0.5) == Voxel::full / 2);

    CHECK(hog::globe::temperature_in_eighths(288.125) == 2'305);
    CHECK(hog::globe::kelvin(hog::globe::temperature_in_eighths(1'473.5)) == 1'473.5);
    CHECK(hog::globe::temperature_in_eighths(-5.0) == 0);
    CHECK(hog::globe::temperature_in_eighths(1e6) == 65'535);
}

TEST_CASE("a generated sample makes shaped ground, open water or air", "[globe][voxel]") {
    const Voxel rock = hog::globe::voxel_from(
        {.distance = -0.75, .material = Material::Granite, .water = 0.0, .temperature = 290.0},
        0.5);
    CHECK(rock.solid_amount == Voxel::full);
    CHECK(rock.solid == Material::Granite);
    CHECK(rock.condition == Voxel::sound);
    CHECK(rock.liquid_amount == 0);
    CHECK(rock.temperature == 2'320);

    const Voxel sea = hog::globe::voxel_from({.distance = 3.0,
                                              .material = Material::Water,
                                              .water = 1.0,
                                              .liquid = Material::Water,
                                              .temperature = 280.0},
                                             0.5);
    CHECK(sea.solid == Material::Air);
    CHECK(sea.solid_amount == 0);
    CHECK(sea.liquid == Material::Water);
    CHECK(sea.liquid_amount == Voxel::full);

    // Ground soaked through holds water in its pores, and is still ground.
    const Voxel seabed = hog::globe::voxel_from({.distance = -0.5,
                                                 .material = Material::Sediment,
                                                 .water = 0.5,
                                                 .liquid = Material::Water,
                                                 .temperature = 280.0},
                                                0.5);
    CHECK(seabed.solid == Material::Sediment);
    CHECK(seabed.liquid == Material::Water);
    CHECK(seabed.liquid_amount == Voxel::full / 2);

    // Far from any surface, air is only air, however far.
    const Voxel air = hog::globe::voxel_from(
        {.distance = 50.0, .material = Material::Air, .water = 0.0, .temperature = 250.0}, 0.5);
    CHECK(air == Voxel{.temperature = 2'000});
}

TEST_CASE("a voxel's value is taken halfway up its layer, mid-column", "[globe][voxel]") {
    const hog::globe::Layers layers = hog::globe::layers_about(115'000.0, 3'000.0, 2'000.0, 0.5);
    const hog::globe::Site site{.face = 2, .i = 1'000, .j = 77, .layer = 6'000};
    const hog::DVec3 at = hog::globe::middle_of(site, layers);
    CHECK(std::abs(glm::length(at) - 115'000.25) < 1e-9);
    CHECK(hog::globe::cell_under(glm::normalize(at), layers.level) ==
          hog::globe::column_of(site, layers));
}

TEST_CASE("a voxel's solid amount is how much of it lies inside the surface", "[globe][voxel]") {
    const hog::globe::Layers layers{.level = 10, .floor = 1'000.0, .thickness = 0.5, .count = 100};
    const auto amount = [&](double distance) {
        hog::globe::Sample sample;
        sample.distance = distance;
        return hog::globe::voxel_from(sample, layers.thickness).solid_amount;
    };
    // Deep in the ground, all of it; high in the air, none; half where the surface passes through
    // the middle.
    CHECK(amount(-10.0) == Voxel::full);
    CHECK(amount(10.0) == 0);
    CHECK(amount(0.0) == Voxel::full / 2);
    // A voxel's thickness takes it from all to none, so a quarter of one under the surface is
    // three quarters full, and the amount falls as the distance grows.
    CHECK(amount(-0.125) > amount(0.0));
    CHECK(amount(0.125) < amount(0.0));
    CHECK(amount(-0.25) == Voxel::full - amount(0.25));
    // Solid at the centre exactly where more than half of it is inside.
    for (const double distance : {-0.4, -0.1, 0.1, 0.4}) {
        CHECK((amount(distance) > Voxel::full / 2) == (distance < 0.0));
    }
}

TEST_CASE("a solid amount and the surface's distance it says go back and forth", "[globe][voxel]") {
    for (const double distance : {-0.24, -0.1, 0.0, 0.1, 0.24}) {
        const std::uint16_t amount = hog::globe::amount_at(distance, 0.5);
        CHECK(std::abs(hog::globe::distance_of(amount, 0.5) - distance) < 1e-4);
    }
    CHECK(hog::globe::amount_at(-1.0, 0.5) == Voxel::full);
    CHECK(hog::globe::amount_at(1.0, 0.5) == 0);
}
