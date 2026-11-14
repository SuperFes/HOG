#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <utility>

using namespace hog::globe;

TEST_CASE("a voxel comes back from a cube with its amounts as they were", "[globe][cube]") {
    for (int amount = 0; std::cmp_less_equal(amount, Voxel::full); amount += 211) {
        Voxel voxel;
        voxel.solid = Material::Granite;
        voxel.solid_amount = static_cast<std::uint16_t>(amount);
        voxel.liquid = Material::Water;
        voxel.liquid_amount = static_cast<std::uint16_t>(Voxel::full - amount);
        voxel.temperature = temperature_in_eighths(285.0);
        const Cube cube = cube_of(voxel, 285.0);
        const Voxel back = voxel_of(cube, 285.0);
        CHECK(back.solid_amount == voxel.solid_amount);
        CHECK(back.liquid_amount == voxel.liquid_amount);
        CHECK(back.temperature == temperature_in_eighths(285.0));
    }
}

TEST_CASE("a material with none of it is none, and alike voxels are alike cubes", "[globe][cube]") {
    Voxel emptied;
    emptied.grain = Material::Sand;
    CHECK(cube_of(emptied, 280.0) == cube_of(Voxel{}, 280.0));
    // However little there is, it's kept.
    Voxel dust = emptied;
    dust.grain_amount = 1;
    CHECK(voxel_of(cube_of(dust, 280.0), 280.0).grain_amount == 1);
    // Whatever the temperature within a step, the same fullness is the same cube.
    Voxel one;
    one.solid = Material::Basalt;
    one.solid_amount = Voxel::full;
    one.temperature = temperature_in_eighths(283.0);
    Voxel other = one;
    other.temperature = temperature_in_eighths(283.5);
    CHECK(cube_of(one, 283.0) == cube_of(other, 283.0));
}

TEST_CASE("heat is kept only over the baseline, in steps", "[globe][cube]") {
    Voxel lava;
    lava.solid = Material::Basalt;
    lava.solid_amount = Voxel::full;
    lava.temperature = temperature_in_eighths(1'500.0);
    const Cube hot = cube_of(lava, 280.0);
    CHECK(hot.heat > 100);
    const Voxel back = voxel_of(hot, 280.0);
    CHECK(std::abs(kelvin(back.temperature) - 1'500.0) <= Cube::heat_step);
    Voxel warm = lava;
    warm.temperature = temperature_in_eighths(283.0);
    CHECK(cube_of(warm, 280.0).heat == 0);
    // And heat is capped, not wrapped.
    Voxel hotter = lava;
    hotter.temperature = temperature_in_eighths(5'000.0);
    CHECK(cube_of(hotter, 280.0).heat == 255);
}

TEST_CASE("water keeps how it rises through the year; what holds none keeps nothing of it",
          "[globe][cube]") {
    Voxel water;
    water.liquid = Material::Water;
    water.liquid_amount = Voxel::full;
    water.rise = 34;
    water.peak = 200;
    const Voxel back = voxel_of(cube_of(water, 280.0), 280.0);
    CHECK(back.rise == 34);
    CHECK(back.peak == 200);
    Voxel drained = water;
    drained.liquid_amount = 0;
    CHECK(cube_of(drained, 280.0) == cube_of(Voxel{}, 280.0));
}
