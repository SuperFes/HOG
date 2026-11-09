#include "test_planet.hpp"

#include <hog/client/globe_view.hpp>
#include <hog/globe/cell.hpp>
#include <hog/jobs/job_system.hpp>
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

// Prints how big the chunks' meshes are by detail, to size what's sent.
