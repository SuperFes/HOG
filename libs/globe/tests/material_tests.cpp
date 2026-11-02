#include <hog/globe/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <numbers>

using hog::globe::Material;
using hog::globe::properties;

TEST_CASE("loose ground has pores and a slope it holds, rock hardly any", "[globe][material]") {
    for (const Material loose :
         {Material::Soil, Material::Sand, Material::Sediment, Material::Regolith, Material::Snow}) {
        CAPTURE(loose);
        CHECK(properties(loose).porosity > 0.3);
        CHECK(properties(loose).porosity < 1.0);
        CHECK(properties(loose).repose > 0.0);
        CHECK(properties(loose).repose < std::numbers::pi / 2.0);
        // Soaked through, a heap holds less steeply than dry.
        CHECK(properties(loose).saturated_repose < properties(loose).repose);
        CHECK(properties(loose).saturated_repose > 0.0);
    }
    for (const Material rock :
         {Material::Granite, Material::Basalt, Material::Shale, Material::Limestone,
          Material::Sandstone, Material::Mantle, Material::Iron, Material::Anorthosite}) {
        CAPTURE(rock);
        CHECK(properties(rock).porosity < 0.2);
        CHECK(properties(rock).repose == 0.0);
    }
    CHECK(properties(Material::Air).porosity == 0.0);
    CHECK(properties(Material::Water).porosity == 0.0);
}

TEST_CASE("water seeps through sand faster than soil, soil faster than mud", "[globe][material]") {
    CHECK(properties(Material::Sand).conductivity > properties(Material::Soil).conductivity);
    CHECK(properties(Material::Soil).conductivity > properties(Material::Sediment).conductivity);
    // Sandstone is an aquifer; crystalline rock is all but tight.
    CHECK(properties(Material::Sandstone).conductivity >
          properties(Material::Granite).conductivity);
    CHECK(properties(Material::Granite).conductivity < 1e-8);
}

TEST_CASE("a flow carries sand before soil, and rock only over ages", "[globe][material]") {
    CHECK(properties(Material::Sand).critical_shear < properties(Material::Soil).critical_shear);
    CHECK(properties(Material::Soil).critical_shear < properties(Material::Granite).critical_shear);
    for (const Material rock : {Material::Granite, Material::Basalt, Material::Sandstone}) {
        CAPTURE(rock);
        CHECK(properties(rock).critical_shear > 100.0);
        CHECK(properties(rock).erodibility < 1e-11);
    }
    for (const Material loose : {Material::Soil, Material::Sand, Material::Sediment}) {
        CAPTURE(loose);
        CHECK(properties(loose).erodibility > 1e-7);
    }
    // Carried, rubble drops first, sand next, mud last.
    CHECK(properties(Material::Granite).settling > properties(Material::Sand).settling);
    CHECK(properties(Material::Sand).settling > properties(Material::Soil).settling);
    CHECK(properties(Material::Soil).settling > properties(Material::Sediment).settling);
    CHECK(properties(Material::Sediment).settling > 0.0);
}
