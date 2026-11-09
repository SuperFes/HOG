#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/tile_sending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <set>
#include <vector>

namespace {

using hog::DVec3;
using hog::globe::Land;
using hog::globe::TileKey;
using hog::server::TileSending;

// Metres the finest tiles are sent within: the tiles a level's ring takes go as its square.
constexpr double reach = 700.0;

// A small world of coarse voxels, so it's cut in few tiles.
const hog::globe::Generator& small() {
    static const hog::globe::Generator generator{[] {
        hog::globe::Planet planet;
        planet.body.radius = 4'000.0;
        planet.voxel = 10.0;
        planet.floor_depth = 200.0;
        planet.sky_height = 1'000.0;
        return planet;
    }()};
    return generator;
}

DVec3 on_ground(DVec3 direction) {
    return glm::normalize(direction) * (small().planet().body.radius + 100.0);
}

DVec3 east_of(DVec3 centre, double metres) {
    const double radius = glm::length(centre);
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, centre));
    return glm::normalize(centre + (east * metres)) * radius;
}

// The tile at `level` holding the finest voxel at (face, i, j, layer).
TileKey holding(std::uint8_t level, std::uint8_t face, std::uint32_t i, std::uint32_t j,
                std::uint32_t layer) {
    const auto shift = static_cast<unsigned>(level + 6);
    return {.level = level, .face = face, .i = i >> shift, .j = j >> shift, .k = layer >> shift};
}

} // namespace

TEST_CASE("a voxel spans from the pixels asked to twice them, however far", "[server][tiles]") {
    constexpr double voxel = 2.5;
    constexpr double pixels = 6.0;
    const TileSending sending{TileSending::reach_for(voxel, pixels)};
    const double texel = 2.0 * std::tan(TileSending::view_fov / 2.0) / TileSending::view_height;
    for (int step = 0; step < 40; ++step) {
        const double metres = TileSending::reach_for(voxel, pixels) * 1.01 * std::pow(1.37, step);
        const std::uint8_t level = sending.level_at(metres, 20);
        const double spans = std::ldexp(voxel, level) / (metres * texel);
        CAPTURE(metres, level);
        CHECK(spans >= pixels * 0.99);
        CHECK(spans <= pixels * 2.0 * 1.01);
    }
}

TEST_CASE("the tiles wanted about a place stand for every place once, finer the nearer",
          "[server][tiles]") {
    Land land{small()};
    const hog::globe::Layers& layers = small().layers();
    const DVec3 here = on_ground({0.3, 0.4, 0.86});
    const TileSending sending{reach};
    const std::vector<TileKey> keys = sending.wanted(here, land);
    const std::set<TileKey> wanted{keys.begin(), keys.end()};
    REQUIRE(wanted.size() == keys.size());

    // Every place is in one of them, and only one.
    std::mt19937 random{7};
    std::uniform_int_distribution<int> faces{0, 5};
    std::uniform_int_distribution<std::uint32_t> across{0, (1U << layers.level) - 1};
    std::uniform_int_distribution<std::uint32_t> up{0,
                                                    static_cast<std::uint32_t>(layers.count - 1)};
    for (int n = 0; n < 2000; ++n) {
        const auto face = static_cast<std::uint8_t>(faces(random));
        const std::uint32_t i = across(random);
        const std::uint32_t j = across(random);
        const std::uint32_t layer = up(random);
        int found = 0;
        for (std::uint8_t level = 0; level <= land.top(); ++level) {
            found += wanted.contains(holding(level, face, i, j, layer)) ? 1 : 0;
        }
        CAPTURE(face, i, j, layer);
        CHECK(found == 1);
    }

    // Nearest first, the finest where the player is, and none finer than its distance asks
    // unless all one.
    CHECK(keys.front().level == 0);
    double last = 0.0;
    for (const TileKey key : keys) {
        const double metres = hog::server::metres_to(layers, key, here);
        CHECK(metres >= last);
        last = metres;
        CHECK((key.level <= sending.level_at(metres, land.top()) || land.hidden(key)));
    }
    CHECK(std::ranges::any_of(keys, [&](TileKey key) { return key.level == land.top(); }));
}

TEST_CASE("a player is sent the tiles about it nearest first, once, then those it comes near",
          "[server][tiles]") {
    Land land{small()};
    TileSending sending{reach};
    const hog::net::ConnectionId player{1};
    sending.join(player);
    const DVec3 here = on_ground({0.3, 0.4, 0.86});
    sending.follow(player, here, land);
    const std::size_t due = sending.due(player);
    REQUIRE(due > 0);

    const hog::protocol::Tiles first = sending.next(player, 1);
    REQUIRE(first.tiles.size() == 1);
    CHECK(first.tiles.front().level == 0);
    std::set<TileKey> sent;
    std::size_t largest = 0;
    auto take = [&](const hog::protocol::Tiles& next) {
        for (const auto& tile : next.tiles) {
            const TileKey key{
                .level = tile.level, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k};
            CHECK(sent.insert(key).second);
            largest = std::max(largest, tile.bytes.size());
            const auto decoded = hog::globe::VoxelTree::decode(tile.bytes);
            REQUIRE(decoded.has_value());
            CHECK(decoded->same_as(land.tile(key)));
        }
    };
    take(first);
    while (sending.due(player) > 0) {
        take(sending.next(player, std::size_t{256} * 1024));
    }
    CHECK(sent.size() == due);
    UNSCOPED_INFO("largest tile " << largest << " bytes of " << sent.size());

    sending.follow(player, here, land);
    CHECK(sending.due(player) == 0);
    sending.follow(player, east_of(here, 4.0 * reach), land);
    REQUIRE(sending.due(player) > 0);
    const hog::protocol::Tiles moved = sending.next(player, std::size_t{1} << 30U);
    for (const auto& tile : moved.tiles) {
        CHECK_FALSE(sent.contains(
            {.level = tile.level, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k}));
    }
}

TEST_CASE("first view cost", "[.bench][server][tiles]") {
    auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const auto started = std::chrono::steady_clock::now();
    const auto seconds = [](auto from) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
    };
    const hog::globe::Generator generator{*planet};
    WARN("generator " << seconds(started) << " s");
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    Land land{generator};
    const TileSending sending{TileSending::reach_for(generator.layers().thickness)};
    const DVec3 up = glm::normalize(DVec3{0.3, 0.4, 0.86});
    const DVec3 here =
        up * (hog::globe::ground_below(generator, up, hog::globe::voxel_lod(generator))
                  .value_or(generator.planet().body.radius) +
              2.0);
    auto at = std::chrono::steady_clock::now();
    const std::vector<TileKey> keys = sending.wanted(here, land, each);
    const double cut = seconds(at);
    std::array<int, 16> per_level{};
    for (const TileKey key : keys) {
        ++per_level.at(key.level);
    }
    at = std::chrono::steady_clock::now();
    std::size_t bytes = 0;
    std::array<double, 16> window_time{};
    std::array<double, 16> encode_time{};
    std::array<std::size_t, 16> quads{};
    std::array<std::size_t, 16> within{};
    for (const TileKey key : keys) {
        auto one = std::chrono::steady_clock::now();
        const auto tree = land.tile(key);
        window_time.at(key.level) += seconds(one);
        one = std::chrono::steady_clock::now();
        bytes += tree.encode().size();
        encode_time.at(key.level) += seconds(one);
        quads.at(key.level) +=
            hog::globe::mesh_tile(generator.layers(), key, tree).indices.size() / 6;
        within.at(key.level) +=
            hog::server::metres_to(generator.layers(), key, here) < 8'000.0 ? 1 : 0;
    }
    for (std::size_t level = 0; level < 8; ++level) {
        WARN("level " << level << ": windows " << window_time.at(level) << " s, encode "
                      << encode_time.at(level) << " s, " << quads.at(level) << " quads, "
                      << within.at(level) << " tiles within 8 km");
    }
    WARN("wanted " << keys.size() << " tiles in " << cut << " s on " << jobs.thread_count()
                   << " threads; windows and bytes " << seconds(at) << " s, " << bytes / 1024
                   << " KiB; per level " << per_level[0] << " " << per_level[1] << " "
                   << per_level[2] << " " << per_level[3] << " " << per_level[4] << " "
                   << per_level[5] << " " << per_level[6] << " " << per_level[7] << " "
                   << per_level[8]);
    // Moving on by a finest tile.
    const DVec3 there =
        glm::normalize(here + (glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, here)) * 200.0)) *
        glm::length(here);
    at = std::chrono::steady_clock::now();
    const auto moved = sending.wanted(there, land, each);
    WARN("moved 200 m: " << moved.size() << " tiles in " << seconds(at) << " s");
}
