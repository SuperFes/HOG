#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using namespace hog::globe;

TEST_CASE("a voxel comes back from a cube as near as the levels allow", "[globe][cube]") {
    for (int amount = 0; amount <= Voxel::full; amount += 211) {
        Voxel voxel;
        voxel.solid = Material::Granite;
        voxel.solid_amount = static_cast<std::uint16_t>(amount);
        voxel.liquid = Material::Water;
        voxel.liquid_amount = static_cast<std::uint16_t>(Voxel::full - amount);
        voxel.temperature = temperature_in_eighths(285.0);
        const Cube cube = cube_of(voxel, 285.0);
        const Voxel back = voxel_of(cube, 285.0, 2.5);
        CHECK(std::abs(static_cast<int>(back.solid_amount) - amount) <= Voxel::full / Cube::levels);
        CHECK(std::abs(static_cast<int>(back.liquid_amount) - (Voxel::full - amount)) <=
              Voxel::full / Cube::levels);
        CHECK(back.temperature == temperature_in_eighths(285.0));
        CHECK(back.normal == Voxel::no_normal);
    }
}

TEST_CASE("a material with too little of it to count is none, and alike voxels are alike cubes",
          "[globe][cube]") {
    Voxel dust;
    dust.grain = Material::Sand;
    dust.grain_amount = 40;
    CHECK(cube_of(dust, 280.0) == cube_of(Voxel{}, 280.0));
    // Whatever the distance, normal and temperature, the same fullness is the same cube.
    Voxel one;
    one.solid = Material::Basalt;
    one.solid_amount = Voxel::full;
    one.distance = -4'000;
    one.normal = packed_normal({0.0, 0.0, 1.0});
    one.temperature = temperature_in_eighths(283.0);
    Voxel other = one;
    other.distance = -9'000;
    other.normal = packed_normal({1.0, 0.0, 0.0});
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
    const Voxel back = voxel_of(hot, 280.0, 2.5);
    CHECK(std::abs(kelvin(back.temperature) - 1'500.0) <= Cube::heat_step);
    Voxel warm = lava;
    warm.temperature = temperature_in_eighths(283.0);
    CHECK(cube_of(warm, 280.0).heat == 0);
    // And heat is capped, not wrapped.
    Voxel hotter = lava;
    hotter.temperature = temperature_in_eighths(5'000.0);
    CHECK(cube_of(hotter, 280.0).heat == 255);
}
