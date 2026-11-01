#include "test_planet.hpp"

#include <hog/client/globe_view.hpp>
#include <hog/client/ground.hpp>
#include <hog/globe/cell.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/materials.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/terrain/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <print>
#include <set>
#include <span>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::globe::Material;
using hog::globe::SurfaceMesh;

TEST_CASE("the globe is a preset or Earth at the world's size, or a planet file",
          "[client][globe]") {
    const auto earth = hog::client::globe_planet(std::nullopt, 7, 90'000.0);
    REQUIRE(earth);
    CHECK(earth->seed == 7);
    CHECK(earth->radius == 90'000.0);
    CHECK(earth->volatiles == hog::globe::Volatile::Water);
    const auto moon = hog::client::globe_planet("moon", 7, 90'000.0);
    REQUIRE(moon);
    CHECK(moon->radius == 90'000.0);
    CHECK(moon->volatiles == hog::globe::Volatile::None);
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

TEST_CASE("a chunk's surface becomes blocks where it lies", "[client][globe]") {
    SurfaceMesh mesh;
    mesh.origin = DVec3{100.0, 20.0, 0.0};
    mesh.vertices = {{.position = Vec3{1.0F, 0.0F, 0.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Granite},
                     {.position = Vec3{0.0F, 1.0F, 0.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Granite},
                     {.position = Vec3{0.0F, 0.0F, 1.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Water}};
    mesh.indices = {2, 1, 0};
    const hog::terrain::BlockMesh blocks = hog::client::globe_blocks(mesh);
    CHECK(blocks.origin.metres() == mesh.origin);
    CHECK(blocks.vertices[0].position == Vec3{1.0F, 0.0F, 0.0F});
    const auto granite = static_cast<std::uint8_t>(hog::client::globe_first_tile +
                                                   static_cast<std::uint8_t>(Material::Granite));
    const auto water = static_cast<std::uint8_t>(hog::client::globe_first_tile +
                                                 static_cast<std::uint8_t>(Material::Water));
    CHECK(blocks.vertices[2].tiles == std::array<std::uint8_t, 3>{water, water, water});
    // Wearing two materials, the triangle gets corners of its own naming both.
    REQUIRE(blocks.vertices.size() == 6);
    CHECK(blocks.indices == std::vector<std::uint32_t>{3, 4, 5});
    CHECK(blocks.vertices[3].tiles == std::array<std::uint8_t, 3>{water, granite, granite});
    CHECK(blocks.vertices[5].corner == 2);
}

TEST_CASE("the blocks' tiles are the volume's materials, then the globe's, each averaging to its "
          "colour",
          "[client][globe]") {
    const std::vector<hog::terrain::MaterialImage> tiles = hog::client::block_tiles();
    REQUIRE(tiles.size() == hog::client::globe_first_tile + hog::globe::material_count);
    CHECK(hog::client::globe_first_tile == hog::planet::material_count);
    for (const hog::terrain::MaterialImage& tile : tiles) {
        CHECK(tile.size == hog::client::material_size);
    }
    for (int material = 0; material < hog::globe::material_count; ++material) {
        const Vec3 colour = hog::client::surface_colour(static_cast<Material>(material));
        const DVec3 mean = hog::terrain::material_mean(
            tiles[hog::client::globe_first_tile + static_cast<std::size_t>(material)]);
        CHECK(glm::length(Vec3{mean} - colour) < 0.02F);
    }
    // The globe's rock varies up close, the volume's plain blocks don't.
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
    CHECK(
        spread(tiles[hog::client::globe_first_tile + static_cast<std::size_t>(Material::Granite)]) >
        40);
    CHECK(spread(tiles[static_cast<std::size_t>(hog::planet::Material::Granite)]) <= 1);
}

namespace {

// Keys shown, as changes come in.
void apply(std::set<std::uint64_t>& shown, const hog::client::GlobeStream::Changes& changes) {
    for (const auto& [key, mesh] : changes.shown) {
        CHECK_FALSE(mesh->indices.empty());
        shown.insert(key);
    }
    for (const std::uint64_t key : changes.gone) {
        CHECK(shown.erase(key) == 1);
    }
}

std::set<std::uint64_t> keys_of(std::span<const hog::globe::Cell> chunks) {
    std::set<std::uint64_t> keys;
    for (const hog::globe::Cell chunk : chunks) {
        keys.insert(hog::client::globe_key(chunk));
    }
    return keys;
}

} // namespace

TEST_CASE("the globe's surfaces follow the eye, coarser further off", "[client][globe]") {
    const hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    hog::client::GlobeStream stream{globe.world, jobs, {.ratio = 1.5, .least = 120.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const auto eye_over = [&](DVec3 at, double height) {
        return at * (radius + hog::client::globe_ground(globe.generator, at) + height);
    };
    std::set<std::uint64_t> shown;
    stream.follow(eye_over(up, 0.0));
    CHECK(stream.in_flight());
    apply(shown, stream.finish());
    CHECK_FALSE(stream.in_flight());
    // Every chunk wanted is shown, at more than one detail.
    CHECK(shown == keys_of(stream.wanted()));
    std::set<int> details;
    for (const hog::globe::Cell chunk : stream.wanted()) {
        details.insert(hog::globe::detail_of(chunk, globe.generator.layers()));
    }
    CHECK(details.size() > 1);

    // Going nowhere much changes nothing.
    stream.follow(eye_over(glm::normalize(up + DVec3{0.0, 0.0, 2.0 / radius}), 0.0));
    CHECK_FALSE(stream.in_flight());
    CHECK(stream.collect().empty());

    // Far off, the chunks left behind are let go, and those beside the new meshed again.
    const DVec3 there = glm::normalize(up + DVec3{0.0, 0.0, 200.0 / radius});
    stream.follow(eye_over(there, 0.0));
    apply(shown, stream.finish());
    CHECK(shown == keys_of(stream.wanted()));
    CHECK(stream.chunks() == shown.size());
}

// Meshes every chunk the client wants at its start, naming each first, so an assertion in the
// mesher points at the chunk. A minute on a release build; run by hand: hog_client_tests "[.gate]".
TEST_CASE("every chunk round the client's start meshes", "[.gate][client][globe]") {
    // The client's own planet, not the tests' smaller one.
    const auto config = hog::planet::load_planet_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                        "config" / "planet.toml")
                            .value();
    const hog::planet::World world{config};
    const double radius = world.geology().planet_radius();
    const hog::client::GlobeWorld globe{
        hog::client::globe_planet(std::nullopt, config.seed, radius).value()};
    const hog::globe::Generator& generator = globe.generator;
    const DVec3 up = hog::planet::start_direction;
    const double height = 30.0;
    const double reach = std::max(1'000.0, std::sqrt((2.0 * radius * height) + (height * height)));
    const std::vector<hog::globe::Cell> chunks =
        hog::globe::chunks_around(generator, {.up = up, .height = height}, reach, 3.0);
    const hog::globe::Drawn drawn{chunks};
    std::println("radius {}, {} chunks", radius, chunks.size());
    for (const hog::globe::Cell chunk : chunks) {
        std::println("chunk level {} face {} i {} j {} detail {}", chunk.level, chunk.face, chunk.i,
                     chunk.j, hog::globe::detail_of(chunk, generator.layers()));
        (void)std::fflush(stdout);
        hog::globe::SurfaceMesh mesh;
        mesh.origin = hog::globe::center(chunk) * radius;
        hog::globe::add_chunk(mesh, globe.world, chunk, drawn);
    }
}

TEST_CASE("a brick changed has the chunks drawn from it meshed again, and no others",
          "[client][globe]") {
    hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    hog::client::GlobeStream stream{globe.world, jobs, {.ratio = 1.5, .least = 120.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const double ground = hog::client::globe_ground(globe.generator, up);
    stream.follow(up * (radius + ground + 2.0));
    std::set<std::uint64_t> shown;
    apply(shown, stream.finish());
    const std::size_t drawn = shown.size();
    REQUIRE(drawn > 4);
    // Dug under the eye, the chunk there is meshed again, and the few beside it.
    const std::uint64_t before = globe.store.revision();
    hog::globe::dig(globe.world, up * (radius + ground), 1.5);
    const std::vector<hog::globe::BrickSite> changed = globe.store.changed_since(before);
    REQUIRE_FALSE(changed.empty());
    for (const hog::globe::BrickSite brick : changed) {
        stream.changed(brick);
    }
    CHECK(stream.in_flight());
    const hog::client::GlobeStream::Changes again = stream.finish();
    CHECK(again.gone.empty());
    REQUIRE_FALSE(again.shown.empty());
    CHECK(again.shown.size() < drawn);
    const hog::globe::Cell under = hog::globe::chunk_under(up, globe.generator.layers(), 0);
    CHECK(std::ranges::any_of(again.shown, [&](const auto& change) {
        return change.first == hog::client::globe_key(under);
    }));
}
