#include <hog/globe/weathering.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::globe::horizons;
using hog::globe::Horizons;
using hog::globe::Material;

TEST_CASE("weathered rock lies topsoil over subsoil over rocky dirt over the rock",
          "[globe][weathering]") {
    const Horizons layers = horizons(Material::Shale, {}, 6.0);
    REQUIRE(layers.topsoil > 0.0);
    CHECK(layers.topsoil < layers.subsoil);
    CHECK(layers.subsoil < layers.depth);
    CHECK(layers.at(0.0) == Material::Soil);
    CHECK(layers.at(layers.topsoil) == Material::Clay);
    CHECK(layers.at(layers.subsoil) == Material::Gravel);
    CHECK(layers.at(layers.depth) == Material::Air);
}

TEST_CASE("only rock weathers", "[globe][weathering]") {
    for (const Material other : {Material::Air, Material::Water, Material::Ice, Material::Soil,
                                 Material::Clay, Material::Iron}) {
        CAPTURE(other);
        CHECK(horizons(other, {}, 6.0).depth == 0.0);
    }
    CHECK(horizons(Material::Granite, {}, 0.0).depth == 0.0);
}

TEST_CASE("warm wet ground rots deep to clay, frozen ground only breaks", "[globe][weathering]") {
    const Horizons tropics = horizons(Material::Basalt, {.kelvin = 300.0, .rain = 2.0}, 6.0);
    const Horizons temperate = horizons(Material::Basalt, {}, 6.0);
    const Horizons tundra = horizons(Material::Basalt, {.kelvin = 255.0, .rain = 0.3}, 6.0);
    CHECK(tropics.depth > temperate.depth);
    CHECK(tropics.subsoil - tropics.topsoil > temperate.subsoil - temperate.topsoil);
    CHECK(tundra.depth < temperate.depth);
    CHECK(tundra.subsoil == 0.0);
    CHECK(tundra.at(0.0) == Material::Gravel);
}

TEST_CASE("deserts weather to sand, and airless worlds to regolith", "[globe][weathering]") {
    CHECK(horizons(Material::Basalt, {.kelvin = 300.0, .rain = 0.1}, 6.0).at(0.0) ==
          Material::Sand);
    const Horizons moon = horizons(Material::Anorthosite, {.liquid = false}, 6.0);
    CHECK(moon.at(0.0) == Material::Regolith);
    CHECK(moon.depth == 6.0);
}

TEST_CASE("each rock rots its own way", "[globe][weathering]") {
    CHECK(horizons(Material::Granite, {}, 6.0).under == Material::Sand);
    CHECK(horizons(Material::Shale, {}, 6.0).under == Material::Clay);
    CHECK(horizons(Material::Limestone, {}, 6.0).depth < horizons(Material::Shale, {}, 6.0).depth);
}
