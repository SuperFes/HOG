#include "test_planet.hpp"

#include <hog/client/tile_meshes.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/tile_sending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using hog::client::GroundChanges;
using hog::client::tile_key;
using hog::client::TileMeshes;
using hog::globe::TileKey;

// Rock a few voxels deep right across a tile.
hog::protocol::SentTile rock_at(TileKey key) {
    const hog::globe::VoxelTree tree =
        hog::globe::VoxelTree::build(hog::globe::Volume::tile_side, [](int, int, int z) {
            return z < 10 ? hog::globe::Cube{.solid = hog::globe::Material::Granite,
                                             .solid_amount = hog::globe::Voxel::full}
                          : hog::globe::Cube{};
        });
    return {.level = key.level,
            .face = key.face,
            .i = key.i,
            .j = key.j,
            .k = key.k,
            .bytes = tree.encode()};
}

// Meshes what's taken, and what's drawn and gone of it.
GroundChanges settle(TileMeshes& meshes) {
    GroundChanges changes = meshes.collect();
    meshes.finish();
    GroundChanges more = meshes.collect();
    changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
    changes.gone.insert(changes.gone.end(), more.gone.begin(), more.gone.end());
    return changes;
}

bool has(const std::vector<GroundChanges::Shown>& shown, TileKey key) {
    return std::ranges::any_of(shown, [&](const auto& one) { return one.key == tile_key(key); });
}

} // namespace

TEST_CASE("a tile is drawn once meshed, until the finer sent after it are drawn all through it",
          "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    const TileKey parent{.level = 1, .face = 2, .i = 3, .j = 3, .k = 0};
    REQUIRE(meshes.take(rock_at(parent)));
    GroundChanges changes = settle(meshes);
    CHECK(has(changes.shown, parent));
    CHECK_FALSE(changes.shown.front().ground->vertices.empty());
    CHECK(meshes.drawn(parent));
    CHECK(meshes.collect().empty());

    // Seven of its eight in: it's drawn still, and they wait.
    for (std::size_t octant = 0; octant < 7; ++octant) {
        REQUIRE(meshes.take(rock_at(parent.child(octant))));
    }
    changes = settle(meshes);
    CHECK(changes.shown.empty());
    CHECK(changes.gone.empty());
    CHECK(meshes.drawn(parent));

    // All eight: they're drawn in its place, and it's let go.
    REQUIRE(meshes.take(rock_at(parent.child(7))));
    changes = settle(meshes);
    CHECK(changes.gone == std::vector{tile_key(parent)});
    CHECK(changes.shown.size() == 8);
    CHECK_FALSE(meshes.holds(parent));
    for (std::size_t octant = 0; octant < 8; ++octant) {
        CHECK(meshes.drawn(parent.child(octant)));
    }

    // Sent again over them, it's drawn in place of all eight.
    REQUIRE(meshes.take(rock_at(parent)));
    changes = settle(meshes);
    CHECK(changes.shown.size() == 1);
    CHECK(has(changes.shown, parent));
    CHECK(changes.gone.size() == 8);
    for (std::size_t octant = 0; octant < 8; ++octant) {
        CHECK_FALSE(meshes.holds(parent.child(octant)));
    }
}

TEST_CASE("a tile is meshed again when a coarser tile beside it comes, reading it at its edge",
          "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    const auto all_of = [](TileKey key, hog::globe::Cube cube) {
        return hog::protocol::SentTile{
            .level = key.level,
            .face = key.face,
            .i = key.i,
            .j = key.j,
            .k = key.k,
            .bytes = hog::globe::VoxelTree{hog::globe::Volume::tile_side, cube}.encode()};
    };
    // Rock to its top, with nothing known over it: it goes on up, so nothing of it shows.
    const TileKey rock{.level = 0, .face = 2, .i = 6, .j = 6, .k = 3};
    REQUIRE(meshes.take(all_of(
        rock, {.solid = hog::globe::Material::Granite, .solid_amount = hog::globe::Voxel::full})));
    GroundChanges changes = settle(meshes);
    REQUIRE(has(changes.shown, rock));
    CHECK(changes.shown.front().ground->vertices.empty());

    // Air sent two levels coarser over it: its top shows.
    const TileKey air{.level = 2, .face = 2, .i = 1, .j = 1, .k = 1};
    REQUIRE(meshes.take(all_of(air, {})));
    changes = settle(meshes);
    const auto shown = std::ranges::find(changes.shown, tile_key(rock), &GroundChanges::Shown::key);
    REQUIRE(shown != changes.shown.end());
    CHECK_FALSE(shown->ground->vertices.empty());
}

TEST_CASE("tiles that aren't, or aren't on the planet, are refused", "[client][tiles]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    hog::jobs::JobSystem jobs{2};
    TileMeshes meshes{made.generator.planet(), made.generator.layers(), jobs};
    hog::protocol::SentTile garbled = rock_at({.face = 1});
    garbled.bytes.resize(3);
    CHECK_FALSE(meshes.take(garbled).has_value());
    CHECK_FALSE(meshes.take(rock_at({.face = 6})).has_value());
    CHECK_FALSE(meshes.take(rock_at({.level = 40, .face = 1})).has_value());
    CHECK(meshes.collect().empty());
}

namespace {

// A tile all one voxel: rock right through, or air.
hog::protocol::SentTile all_of(TileKey key, bool rock) {
    const hog::globe::VoxelTree tree =
        hog::globe::VoxelTree::build(hog::globe::Volume::tile_side, [&](int, int, int) {
            return rock ? hog::globe::Cube{.solid = hog::globe::Material::Granite,
                                           .solid_amount = hog::globe::Voxel::full}
                        : hog::globe::Cube{};
        });
    return {.level = key.level,
            .face = key.face,
            .i = key.i,
            .j = key.j,
            .k = key.k,
            .bytes = tree.encode()};
}

} // namespace

TEST_CASE("a tile sent again after one over it gives way to it once all of its place comes",
          "[client][tiles]") {
    hog::jobs::JobSystem jobs;
    const auto& generator = hog::client::testing::globe().generator;
    TileMeshes meshes{generator.planet(), generator.layers(), jobs};
    const TileKey coarse{.level = 1, .face = 1, .i = 40, .j = 40, .k = 1};
    // A tile, then the one over it, drawn in its place; then all of that place finer again,
    // the first among it.
    REQUIRE(meshes.take(rock_at(coarse.child(0))));
    static_cast<void>(settle(meshes));
    REQUIRE(meshes.take(rock_at(coarse)));
    static_cast<void>(settle(meshes));
    CHECK(meshes.drawn(coarse));
    for (std::size_t octant = 0; octant < 8; ++octant) {
        REQUIRE(meshes.take(rock_at(coarse.child(octant))));
    }
    static_cast<void>(settle(meshes));
    static_cast<void>(settle(meshes));
    CHECK_FALSE(meshes.holds(coarse));
    for (std::size_t octant = 0; octant < 8; ++octant) {
        CHECK(meshes.drawn(coarse.child(octant)));
    }
}

TEST_CASE("a tile is meshed again when a coarser tile it read beside it is dropped",
          "[client][tiles]") {
    hog::jobs::JobSystem jobs;
    const auto& generator = hog::client::testing::globe().generator;
    TileMeshes meshes{generator.planet(), generator.layers(), jobs};
    // Rock, and over it air a level coarser; then the air's place sent finer than the rock, as
    // rock all through.
    const TileKey rock{.level = 1, .face = 1, .i = 40, .j = 40, .k = 3};
    const TileKey air{.level = 2, .face = 1, .i = 20, .j = 20, .k = 2};
    REQUIRE(meshes.take(all_of(rock, true)));
    REQUIRE(meshes.take(all_of(air, false)));
    const auto drawn = [&](const GroundChanges& changes) -> std::optional<std::size_t> {
        std::optional<std::size_t> indices;
        for (const auto& shown : changes.shown) {
            if (shown.key == tile_key(rock)) {
                indices = shown.ground->indices.size();
            }
        }
        return indices;
    };
    const std::optional<std::size_t> lid = drawn(settle(meshes));
    REQUIRE(lid);
    CHECK(lid.value_or(0) > 0);
    for (std::uint32_t i = 0; i < 4; ++i) {
        for (std::uint32_t j = 0; j < 4; ++j) {
            for (std::uint32_t k = 0; k < 4; ++k) {
                REQUIRE(meshes.take(
                    all_of({.level = 0, .face = 1, .i = 80 + i, .j = 80 + j, .k = 8 + k}, true)));
            }
        }
    }
    GroundChanges changes = settle(meshes);
    const GroundChanges more = settle(meshes);
    changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
    CHECK_FALSE(meshes.holds(air));
    // Against rock now all round, it shows nothing.
    const std::optional<std::size_t> after = drawn(changes);
    REQUIRE(after);
    CHECK(after.value_or(1) == 0);
}

namespace {

// A client flown through `eyes`, cut for and sent the land at each as the server does, checking
// after each that it draws each place once, as the server last cut it.
void fly_through(const std::vector<hog::DVec3>& eyes) {
    const hog::globe::Generator& generator = hog::client::testing::globe().generator;
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t /*thread*/) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    hog::globe::Land land{generator};
    const hog::server::TileSending sending{
        hog::server::TileSending::reach_for(generator.layers().thickness)};
    TileMeshes meshes{generator.planet(), generator.layers(), jobs};
    std::map<std::uint64_t, TileKey> keys;
    std::set<std::uint64_t> drawn;
    std::set<TileKey> held;
    for (std::size_t step = 0; step < eyes.size(); ++step) {
        const hog::server::TileSending::Cut cut = sending.cut(eyes[step], held, land, each);
        CHECK(cut.too_big == 0);
        held = {cut.keys.begin(), cut.keys.end()};
        for (const hog::protocol::SentTile& tile : cut.sent) {
            const TileKey key{
                .level = tile.level, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k};
            keys.insert_or_assign(tile_key(key), key);
            REQUIRE(meshes.take(tile));
        }
        const GroundChanges changes = settle(meshes);
        for (const std::uint64_t gone : changes.gone) {
            drawn.erase(gone);
        }
        for (const auto& shown : changes.shown) {
            drawn.insert(shown.key);
        }
        // Nothing drawn over what's drawn finer, and nothing drawn the server no longer cuts.
        std::size_t stale = 0;
        std::size_t over = 0;
        for (const std::uint64_t one : drawn) {
            const TileKey key = keys.at(one);
            stale += held.contains(key) ? 0 : 1;
            for (TileKey parent = key; parent.level < land.top();) {
                parent = parent.parent();
                over += drawn.contains(tile_key(parent)) ? 1 : 0;
            }
        }
        CAPTURE(step, drawn.size(), held.size(), cut.sent.size());
        CHECK(stale == 0);
        CHECK(over == 0);
    }
}

// Of the tiles drawn, those not among the server's last cut, and those drawn over others drawn
// finer.
struct Drawn {
    std::size_t stale = 0;
    std::size_t over = 0;
};
Drawn drawn_against(const std::set<std::uint64_t>& drawn,
                    const std::map<std::uint64_t, TileKey>& keys, const std::set<TileKey>& held,
                    std::uint8_t top) {
    Drawn counted;
    for (const std::uint64_t one : drawn) {
        const TileKey key = keys.at(one);
        counted.stale += held.contains(key) ? 0 : 1;
        for (TileKey parent = key; parent.level < top;) {
            parent = parent.parent();
            counted.over += drawn.contains(tile_key(parent)) ? 1 : 0;
        }
    }
    return counted;
}

// The point `metres` over the ground at `up`.
hog::DVec3 over_ground(hog::DVec3 up, double metres) {
    const hog::globe::Generator& generator = hog::client::testing::globe().generator;
    const double ground = hog::globe::ground_below(generator, up, hog::globe::voxel_lod(generator))
                              .value_or(generator.planet().radius);
    return up * (ground + metres);
}

} // namespace

TEST_CASE("a client flying in from far and high draws each place once, as last cut",
          "[client][tiles]") {
    // From fifteen kilometres off and three high down to sixty metres over the land.
    const double radius = hog::client::testing::globe().generator.planet().radius;
    const hog::DVec3 end = hog::client::testing::on_land();
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, end));
    constexpr int steps = 30;
    std::vector<hog::DVec3> eyes;
    eyes.reserve(steps + 1);
    for (int step = 0; step <= steps; ++step) {
        const double left = 1.0 - (static_cast<double>(step) / steps);
        eyes.push_back(over_ground(glm::normalize(end - (east * (15'000.0 * left / radius))),
                                   60.0 + (2'940.0 * left)));
    }
    fly_through(eyes);
}

TEST_CASE("a client flown high over the planet and back down settles on the last cut",
          "[client][tiles]") {
    const hog::globe::Generator& generator = hog::client::testing::globe().generator;
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t /*thread*/) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    hog::globe::Land land{generator};
    hog::server::TileSending sending{
        hog::server::TileSending::reach_for(generator.layers().thickness)};
    constexpr hog::net::ConnectionId player{1};
    sending.join(player);
    TileMeshes meshes{generator.planet(), generator.layers(), jobs};
    std::map<std::uint64_t, TileKey> keys;
    std::set<std::uint64_t> drawn;
    // As the server does each tick: a cut finished taken; one begun where the player's moved, on
    // what it held then, done some ticks later; and the next tiles sent meanwhile.
    std::optional<hog::server::TileSending::Cut> cutting;
    int ready = 0;
    const auto tick = [&](hog::DVec3 at) {
        if (cutting && --ready <= 0) {
            sending.take(player, std::move(*cutting));
            cutting.reset();
        }
        if (!cutting && sending.moved(player, at, generator.layers())) {
            cutting = sending.cut(at, sending.held(player), land, each);
            ready = 2;
        }
        for (const hog::protocol::SentTile& tile :
             sending.next(player, std::size_t{256} * 1024).tiles) {
            const TileKey key{
                .level = tile.level, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k};
            keys.insert_or_assign(tile_key(key), key);
            REQUIRE(meshes.take(tile));
        }
        const GroundChanges changes = settle(meshes);
        for (const std::uint64_t gone : changes.gone) {
            drawn.erase(gone);
        }
        for (const auto& shown : changes.shown) {
            drawn.insert(shown.key);
        }
    };

    // Up from the land to fifteen kilometres over ten across, and back down to where it began.
    const double radius = generator.planet().radius;
    const hog::DVec3 start = hog::client::testing::on_land();
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, start));
    const auto at = [&](double share) {
        const double arc = 10'000.0 * std::sin(share * std::numbers::pi);
        return over_ground(glm::normalize(start + (east * (arc / radius))),
                           60.0 + (15'000.0 * std::sin(share * std::numbers::pi)));
    };
    constexpr int ticks = 60;
    for (int n = 0; n <= ticks; ++n) {
        tick(at(static_cast<double>(n) / ticks));
    }
    for (int n = 0; n < 4 || cutting || sending.due(player) > 0; ++n) {
        tick(at(1.0));
    }
    const Drawn counted = drawn_against(drawn, keys, sending.held(player), land.top());
    CAPTURE(drawn.size());
    CHECK(counted.stale == 0);
    CHECK(counted.over == 0);
}
