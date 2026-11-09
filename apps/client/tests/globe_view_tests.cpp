#include "test_planet.hpp"

#include <hog/client/globe_view.hpp>
#include <hog/globe/cell.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/terrain/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::globe::Material;

TEST_CASE("the globe is the default knobs at the world's size, or a planet file",
          "[client][globe]") {
    const auto earth = hog::client::globe_planet(std::nullopt, 7, 90'000.0);
    REQUIRE(earth);
    CHECK(earth->seed == 7);
    CHECK(earth->radius == 90'000.0);
    CHECK(earth->volatiles == hog::globe::Volatile::Water);
    CHECK_FALSE(hog::client::globe_planet("no such planet.toml", 7, 90'000.0));
}

TEST_CASE("every material has a colour of its own", "[client][globe]") {
    std::set<std::array<float, 3>> colours;
    for (int material = 0; material < hog::globe::material_count; ++material) {
        const Vec3 colour = hog::client::surface_colour(static_cast<Material>(material));
        CHECK(colour != Vec3{1.0F, 0.0F, 1.0F});
        colours.insert({colour.x, colour.y, colour.z});
    }
    CHECK(colours.size() == static_cast<std::size_t>(hog::globe::material_count));
}

TEST_CASE("the blocks' tiles are the globe's materials, each averaging to its colour",
          "[client][globe]") {
    const std::vector<hog::terrain::MaterialImage> tiles = hog::client::block_tiles();
    REQUIRE(tiles.size() == static_cast<std::size_t>(hog::globe::material_count));
    for (const hog::terrain::MaterialImage& tile : tiles) {
        CHECK(tile.size == hog::client::material_size);
    }
    for (int material = 0; material < hog::globe::material_count; ++material) {
        const Vec3 colour = hog::client::surface_colour(static_cast<Material>(material));
        const DVec3 mean = hog::terrain::material_mean(tiles[static_cast<std::size_t>(material)]);
        CHECK(glm::length(Vec3{mean} - colour) < 0.02F);
    }
    // The rock varies up close.
    const auto spread = [](const hog::terrain::MaterialImage& tile) {
        // The red channel of each texel.
        std::uint8_t least = 255;
        std::uint8_t most = 0;
        for (std::size_t texel = 0; texel < tile.levels.front().size(); texel += 4) {
            least = std::min(least, tile.levels.front()[texel]);
            most = std::max(most, tile.levels.front()[texel]);
        }
        return most - least;
    };
    CHECK(spread(tiles[static_cast<std::size_t>(Material::Granite)]) > 40);
}

TEST_CASE("a planet file keeps its own size and takes the config's seed", "[globe_view]") {
    const auto path = std::filesystem::temp_directory_path() / "hog_globe_view_planet.toml";
    {
        std::ofstream{path} << "[globe]\nradius = 868500.0\n";
    }
    const auto planet = hog::client::globe_planet(path.string(), 7, 115'000.0);
    std::filesystem::remove(path);
    REQUIRE(planet.has_value());
    CHECK(planet->radius == 868'500.0);
    CHECK(planet->seed == 7);
    // With no file, the default knobs at the world's own size.
    const auto unnamed = hog::client::globe_planet(std::nullopt, 7, 115'000.0);
    REQUIRE(unnamed.has_value());
    CHECK(unnamed->radius == 115'000.0);
    CHECK(unnamed->seed == 7);
}

TEST_CASE("a planet file's [globe] table is the voxel world drawn, at the world's seed",
          "[client][globe]") {
    const auto path = std::filesystem::temp_directory_path() / "hog_globe_view_in_planet.toml";
    std::ofstream{path}
        << "[planet]\nseed = 1\n\n[globe]\nradius = 12000.0\n"
           "floor_depth = 200.0\nsky_height = 800.0\nvoxel = 1.5\nformed_detail = 6\n";
    const auto planet = hog::client::globe_planet_in(path, 9, 115'000.0);
    std::filesystem::remove(path);
    REQUIRE(planet);
    CHECK(planet->seed == 9);
    CHECK(planet->radius == 12'000.0);
    CHECK(planet->voxel == 1.5);
    CHECK(planet->formed_detail == 6.0);
    const auto none = hog::client::globe_planet_in(path, 9, 90'000.0);
    REQUIRE(none);
    CHECK(none->radius == 90'000.0);
}

// Prints how big the chunks' meshes are by detail, to size what's sent.
