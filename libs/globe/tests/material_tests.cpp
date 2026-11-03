#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using hog::globe::Material;
using hog::globe::holding_slope;
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

TEST_CASE("a layer of loose ground holds a steeper slope thin than deep, and slides sooner wet",
          "[globe][material]") {
    constexpr double gravity = 9.81;
    const auto deg = [](double degrees) { return std::tan(degrees * std::numbers::pi / 180.0); };

    // Dry sand deep on sand holds its repose.
    CHECK(holding_slope(Material::Sand, Material::Sand, 0.0, 5.0, gravity) ==
          Catch::Approx(deg(33.0)).epsilon(0.01));
    // A coating on bare rock slides sooner than the heap, soaked sooner still.
    const double on_rock = holding_slope(Material::Sand, Material::Granite, 0.0, 0.0, gravity);
    CHECK(on_rock == Catch::Approx(deg(28.0)).epsilon(0.01));
    CHECK(holding_slope(Material::Sand, Material::Granite, 1.0, 0.0, gravity) < on_rock);
    // Wet rock is slicker than dry, and clay shale the slickest.
    CHECK(holding_slope(Material::Soil, Material::Granite, 0.0, 0.0, gravity) >
          holding_slope(Material::Soil, Material::Shale, 1.0, 0.0, gravity));
    // Snow holds on in thinner layers than thick ones, and on a cliff only a dusting stays.
    double before = 1e9;
    for (const double depth : {0.01, 0.05, 0.2, 1.0, 5.0}) {
        const double holds = holding_slope(Material::Snow, Material::Granite, 0.0, depth, gravity);
        CHECK(holds < before);
        before = holds;
    }
    CHECK(holding_slope(Material::Snow, Material::Granite, 0.0, 0.02, gravity) > deg(60.0));
    CHECK(holding_slope(Material::Snow, Material::Granite, 0.0, 5.0, gravity) < deg(60.0));
    // Between dry and soaked it eases off steadily.
    const double dry = holding_slope(Material::Soil, Material::Soil, 0.0, 5.0, gravity);
    const double damp = holding_slope(Material::Soil, Material::Soil, 0.5, 5.0, gravity);
    const double mud = holding_slope(Material::Soil, Material::Soil, 1.0, 5.0, gravity);
    CHECK(dry > damp);
    CHECK(damp > mud);
    // Rubble has no repose of its own and still holds a heap.
    CHECK(holding_slope(Material::Basalt, Material::Basalt, 0.0, 5.0, gravity) ==
          Catch::Approx(deg(40.0)).epsilon(0.01));
}

TEST_CASE("a little water in the ground wets its surface, and the same depth in the open does too",
          "[globe][material]") {
    using hog::globe::Voxel;
    const double thickness = 1.0;
    const auto soil = Material::Soil;
    const auto holding = [&](double metres) {
        return static_cast<std::uint16_t>(metres * Voxel::full);
    };
    // Dry stays dry; a few millimetres only dampen; a few centimetres soak the surface.
    CHECK(hog::globe::dampness(soil, 0, thickness) == 0.0);
    const double dew = hog::globe::dampness(soil, holding(0.004), thickness);
    CHECK(dew > 0.0);
    CHECK(dew < 0.5);
    CHECK(hog::globe::dampness(soil, holding(0.04), thickness) == 1.0);
    // Open water as deep as the skin is soaked, a film thinner than that less so.
    CHECK(hog::globe::dampness(Material::Air, holding(0.06), thickness) == 1.0);
    CHECK(hog::globe::dampness(Material::Air, holding(0.01), thickness) < 0.5);
    // A coarser voxel spreads the same share over more depth, so wets more.
    CHECK(hog::globe::dampness(soil, holding(0.004), 4.0) > dew);
}
