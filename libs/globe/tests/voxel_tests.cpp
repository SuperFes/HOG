#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

using hog::globe::Material;
using hog::globe::Voxel;

TEST_CASE("voxels hold distance, amounts and temperature in fixed steps", "[globe][voxel]") {
    CHECK(hog::globe::distance_in_mm(0.0) == 0);
    CHECK(hog::globe::distance_in_mm(1.2344) == 1'234);
    CHECK(hog::globe::distance_in_mm(-1.2346) == -1'235);
    CHECK(hog::globe::distance_in_mm(1e9) == Voxel::far);
    CHECK(hog::globe::distance_in_mm(-1e9) == -Voxel::far);
    CHECK(hog::globe::metres(hog::globe::distance_in_mm(2.5)) == 2.5);

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

TEST_CASE("a normal packs to within a degree, and none stays none", "[globe][voxel]") {
    CHECK(glm::length(hog::globe::unpacked_normal(Voxel::no_normal)) == 0.0);
    CHECK(hog::globe::packed_normal(hog::DVec3{0.0}) == Voxel::no_normal);
    double worst = 0.0;
    for (int a = 0; a <= 72; ++a) {
        for (int b = 0; b <= 36; ++b) {
            const double around = a * std::numbers::pi / 36.0;
            const double down = b * std::numbers::pi / 36.0;
            const hog::DVec3 unit{std::sin(down) * std::cos(around),
                                  std::sin(down) * std::sin(around), std::cos(down)};
            const std::uint16_t packed = hog::globe::packed_normal(unit);
            CHECK(packed != Voxel::no_normal);
            const hog::DVec3 back = hog::globe::unpacked_normal(packed);
            CHECK(std::abs(glm::length(back) - 1.0) < 1e-12);
            worst = std::max(worst, std::acos(std::min(glm::dot(unit, back), 1.0)));
        }
    }
    CHECK(worst < std::numbers::pi / 180.0);
}

TEST_CASE("a generated sample makes shaped ground, open water or air", "[globe][voxel]") {
    const hog::DVec3 up{0.0, 0.0, 1.0};
    const Voxel rock = hog::globe::voxel_from({.distance = -0.75,
                                               .material = Material::Granite,
                                               .water = 0.0,
                                               .normal = up,
                                               .temperature = 290.0},
                                              2.0);
    CHECK(rock.distance == -750);
    CHECK(rock.normal == hog::globe::packed_normal(up));
    CHECK(rock.solid == Material::Granite);
    CHECK(rock.condition == Voxel::sound);
    CHECK(rock.liquid_amount == 0);
    CHECK(rock.temperature == 2'320);

    const Voxel sea = hog::globe::voxel_from({.distance = 3.0,
                                              .material = Material::Water,
                                              .water = 1.0,
                                              .liquid = Material::Water,
                                              .temperature = 280.0},
                                             2.0);
    CHECK(sea.solid == Material::Air);
    CHECK(sea.liquid == Material::Water);
    CHECK(sea.liquid_amount == Voxel::full);

    // Ground soaked through holds water in its pores, and is still ground.
    const Voxel seabed = hog::globe::voxel_from({.distance = -0.5,
                                                 .material = Material::Sediment,
                                                 .water = 0.5,
                                                 .liquid = Material::Water,
                                                 .normal = up,
                                                 .temperature = 280.0},
                                                2.0);
    CHECK(seabed.solid == Material::Sediment);
    CHECK(seabed.liquid == Material::Water);
    CHECK(seabed.liquid_amount == Voxel::full / 2);

    // Out of the band, only which side it's on is kept.
    const Voxel air = hog::globe::voxel_from({.distance = 50.0,
                                              .material = Material::Air,
                                              .water = 0.0,
                                              .normal = up,
                                              .temperature = 250.0},
                                             2.0);
    CHECK(air == Voxel{.distance = 2'000, .temperature = 2'000});
    CHECK(sea.distance == 2'000);
}

TEST_CASE("a voxel's value is taken halfway up its layer, mid-column", "[globe][voxel]") {
    const hog::globe::Layers layers = hog::globe::layers_about(115'000.0, 3'000.0, 2'000.0, 0.5);
    const hog::globe::Site site{.face = 2, .i = 1'000, .j = 77, .layer = 6'000};
    const hog::DVec3 at = hog::globe::middle_of(site, layers);
    CHECK(std::abs(glm::length(at) - 115'000.25) < 1e-9);
    CHECK(hog::globe::cell_under(glm::normalize(at), layers.level) ==
          hog::globe::column_of(site, layers));
}
