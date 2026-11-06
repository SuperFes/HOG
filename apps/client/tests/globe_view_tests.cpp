#include "test_planet.hpp"

#include <hog/chunks/globe_stream.hpp>
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
                      .material = Material::Granite,
                      .wet = 1.0F},
                     {.position = Vec3{0.0F, 1.0F, 0.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Granite},
                     {.position = Vec3{0.0F, 0.0F, 1.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Water}};
    mesh.indices = {2, 1, 0};
    const hog::terrain::BlockMesh blocks = hog::chunks::globe_blocks(mesh);
    CHECK(blocks.origin.metres() == mesh.origin);
    CHECK(blocks.vertices[0].position == Vec3{1.0F, 0.0F, 0.0F});
    const auto granite = static_cast<std::uint8_t>(Material::Granite);
    const auto water = static_cast<std::uint8_t>(Material::Water);
    CHECK(blocks.vertices[2].tiles == std::array<std::uint8_t, 3>{water, water, water});
    CHECK(hog::terrain::wetness_of(blocks.vertices[0].corner) == 1.0);
    CHECK(hog::terrain::wetness_of(blocks.vertices[1].corner) == 0.0);
    // Wearing two materials, the triangle gets corners of its own naming both, each as wet as
    // the vertex it stands for.
    REQUIRE(blocks.vertices.size() == 6);
    CHECK(blocks.indices == std::vector<std::uint32_t>{3, 4, 5});
    CHECK(blocks.vertices[3].tiles == std::array<std::uint8_t, 3>{water, granite, granite});
    CHECK(hog::terrain::corner_of(blocks.vertices[5].corner) == 2);
    CHECK(hog::terrain::wetness_of(blocks.vertices[5].corner) == 1.0);
}

TEST_CASE("the liquid's surface is left out of a chunk's blocks", "[client][globe]") {
    SurfaceMesh mesh;
    for (const Vec3 position :
         {Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F}}) {
        mesh.vertices.push_back(
            {.position = position, .normal = Vec3{0.0F, 0.0F, 1.0F}, .material = Material::Water});
    }
    mesh.indices = {0, 1, 2};
    const hog::terrain::BlockMesh blocks = hog::chunks::globe_blocks(mesh);
    CHECK(blocks.indices.empty());
}

TEST_CASE("liquid standing clear of the sea's level is still water for the sea to draw",
          "[client][globe]") {
    constexpr double sea = 115'000.0;
    SurfaceMesh mesh;
    mesh.origin = DVec3{0.0, 0.0, sea};
    // One triangle at the sea's level, one a hundred metres up, one of ground beside it.
    for (const float height : {0.0F, 100.0F}) {
        for (const Vec3 across :
             {Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}}) {
            mesh.vertices.push_back({.position = across + Vec3{0.0F, 0.0F, height},
                                     .normal = Vec3{0.0F, 0.0F, 1.0F},
                                     .material = Material::Water});
        }
    }
    for (const Vec3 across :
         {Vec3{3.0F, 0.0F, 0.0F}, Vec3{2.0F, 1.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}}) {
        mesh.vertices.push_back({.position = across + Vec3{0.0F, 0.0F, 100.0F},
                                 .normal = Vec3{0.0F, 0.0F, 1.0F},
                                 .material = Material::Soil});
    }
    mesh.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    const hog::terrain::BlockMesh still = hog::chunks::globe_still_water(mesh, sea, 0.5);
    REQUIRE(still.indices == std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(still.vertices.size() == 3);
    CHECK(still.vertices[0].position == Vec3{1.0F, 0.0F, 100.0F});
    CHECK(still.origin.metres() == mesh.origin);
    const auto water = static_cast<std::uint8_t>(Material::Water);
    CHECK(still.vertices[1].tiles == std::array<std::uint8_t, 3>{water, water, water});
    // Without a sea, all liquid is still.
    CHECK(hog::chunks::globe_still_water(mesh, std::nullopt, 0.5).indices.size() == 6);
    CHECK(hog::chunks::globe_blocks(mesh).indices.size() == 3);
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

namespace {

// Keys shown, as changes come in.
void apply(std::set<std::uint64_t>& shown, const hog::chunks::GlobeStream::Changes& changes) {
    for (const auto& [key, ground, water] : changes.shown) {
        CHECK_FALSE(ground->indices.empty());
        shown.insert(key);
    }
    for (const std::uint64_t key : changes.gone) {
        CHECK(shown.erase(key) == 1);
    }
}

std::set<std::uint64_t> keys_of(std::span<const hog::globe::Cell> chunks) {
    std::set<std::uint64_t> keys;
    for (const hog::globe::Cell chunk : chunks) {
        keys.insert(hog::chunks::globe_key(chunk));
    }
    return keys;
}

} // namespace

TEST_CASE("the globe's surfaces follow the eye, coarser further off", "[client][globe]") {
    const hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    hog::chunks::GlobeStream stream{globe.world, jobs, {.ratio = 1.5, .least = 120.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const auto eye_over = [&](DVec3 at, double height) {
        return at * (radius + hog::chunks::globe_ground(globe.world, at) + height);
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
    const double radius = config.radius;
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
    hog::chunks::GlobeStream stream{globe.world, jobs, {.ratio = 1.5, .least = 120.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const double ground = hog::chunks::globe_ground(globe.world, up);
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
    const hog::chunks::GlobeStream::Changes again = stream.finish();
    CHECK(again.gone.empty());
    REQUIRE_FALSE(again.shown.empty());
    CHECK(again.shown.size() < drawn);
    const hog::globe::Cell under = hog::globe::chunk_under(up, globe.generator.layers(), 0);
    CHECK(std::ranges::any_of(again.shown, [&](const auto& change) {
        return change.key == hog::chunks::globe_key(under);
    }));
}

TEST_CASE("a planet file keeps its own size and takes the config's seed", "[globe_view]") {
    const auto path = std::filesystem::temp_directory_path() / "hog_globe_view_planet.toml";
    {
        std::ofstream{path} << "[globe]\npreset = \"moon\"\nradius = 868500.0\n";
    }
    const auto planet = hog::client::globe_planet(path.string(), 7, 115'000.0);
    std::filesystem::remove(path);
    REQUIRE(planet.has_value());
    CHECK(planet->radius == 868'500.0);
    CHECK(planet->seed == 7);
    const auto preset = hog::client::globe_planet("moon", 7, 115'000.0);
    REQUIRE(preset.has_value());
    CHECK(preset->radius == 115'000.0);
}

TEST_CASE("a planet file's [globe] table is the voxel world drawn, at the world's seed",
          "[client][globe]") {
    const auto path = std::filesystem::temp_directory_path() / "hog_globe_view_in_planet.toml";
    std::ofstream{path} << "[planet]\nseed = 1\n\n[globe]\npreset = \"earth\"\nradius = 12000.0\n"
                           "floor_depth = 200.0\nsky_height = 800.0\nvoxel = 1.5\nwear = 0.02\n";
    const auto planet = hog::client::globe_planet_in(path, 9, 115'000.0);
    std::filesystem::remove(path);
    REQUIRE(planet);
    CHECK(planet->seed == 9);
    CHECK(planet->radius == 12'000.0);
    CHECK(planet->voxel == 1.5);
    CHECK(planet->wear == 0.02);
    const auto none = hog::client::globe_planet_in(path, 9, 90'000.0);
    REQUIRE(none);
    CHECK(none->radius == 90'000.0);
}

// Prints how big the chunks' meshes are by detail, to size what's sent.
TEST_CASE("how big the chunks' meshes are, to look at", "[.look][client][globe]") {
    const hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    hog::chunks::GlobeStream stream{globe.world, jobs, {.ratio = 3.0, .least = 1'000.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    stream.follow(up * (radius + hog::chunks::globe_ground(globe.world, up) + 2.0));
    const auto changes = stream.finish();
    std::map<std::uint64_t, int> detail_of;
    for (const hog::globe::Cell chunk : stream.wanted()) {
        detail_of[hog::chunks::globe_key(chunk)] = hog::globe::detail_of(chunk, globe.generator.layers());
    }
    struct Size {
        std::size_t count = 0, vertices = 0, indices = 0, most_vertices = 0, most_indices = 0, water = 0;
    };
    std::map<int, Size> by;
    for (const auto& shown : changes.shown) {
        Size& size = by[detail_of[shown.key]];
        ++size.count;
        size.vertices += shown.ground->vertices.size();
        size.indices += shown.ground->indices.size();
        size.most_vertices = std::max(size.most_vertices, shown.ground->vertices.size());
        size.most_indices = std::max(size.most_indices, shown.ground->indices.size());
        size.water = std::max(size.water, shown.water->vertices.size());
    }
    for (const auto& [detail, size] : by) {
        std::println("detail {:2}: {:4} chunks, mean {:6} v {:6} i, most {:6} v {:6} i, water most {}",
                     detail, size.count, size.vertices / size.count, size.indices / size.count,
                     size.most_vertices, size.most_indices, size.water);
    }
}
