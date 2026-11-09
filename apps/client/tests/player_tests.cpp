#include "test_planet.hpp"

#include <hog/client/flight.hpp>
#include <hog/client/player.hpp>
#include <hog/client/scene.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/world.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <cstdint>
#include <memory>
#include <utility>

using Catch::Approx;
using namespace hog;
using namespace hog::client;

namespace {

DVec3 on_land() {
    return client::testing::on_land();
}

double radius() {
    return client::testing::globe().generator.planet().body.radius;
}

// Metres the ground stands over the radius at a direction.
double ground_at(DVec3 direction) {
    const globe::Generator& generator = client::testing::globe().generator;
    return globe::ground_below(generator, direction, globe::voxel_lod(generator)).value_or(0.0) -
           radius();
}

double height_of(WorldPosition at) {
    return glm::length(at.metres()) - radius();
}

Player standing() {
    auto player = Player::create(client::testing::globe().world, on_land());
    REQUIRE(player);
    REQUIRE(player->play({}, 1.0));
    return std::move(player).value();
}

// Facing along the ground from the player.
Flight aim(const Player& player) {
    return start_flight(glm::normalize(player.feet().metres()), radius(), 0.0, planet::north_pole);
}

// Metres between two points on the ground, along it.
double apart(WorldPosition a, WorldPosition b) {
    const DVec3 up = glm::normalize(a.metres());
    const DVec3 between = b - a;
    return glm::length(between - (up * glm::dot(between, up)));
}

} // namespace

TEST_CASE("a player starts standing on the ground under them", "[client][player]") {
    const double ground = ground_at(on_land());
    REQUIRE(ground > 0.0);
    Player player = standing();
    CHECK(player.standing());
    CHECK(height_of(player.feet()) == Approx(ground).margin(2.0));
    // Settling where it stands, though hardly moving.
    CHECK(apart(player.feet(), WorldPosition{on_land() * radius()}) < 0.5);

    const WorldPosition before = player.feet();
    REQUIRE(player.play({}, 1.0));
    CHECK(glm::length(player.feet() - before) < 0.05);
}

TEST_CASE("a player walks or runs the way they face", "[client][player]") {
    // Briefly: a few metres on, the ground at the spot rises too steeply to run up.
    Player walker = standing();
    const WorldPosition start = walker.feet();
    const Flight facing = aim(walker);
    REQUIRE(walker.play(stride(facing, {0.0, 0.0, 1.0}, false), 0.4));
    const double walked = apart(start, walker.feet());
    CHECK(walked == Approx(0.6).margin(0.15));
    CHECK(glm::dot(glm::normalize(walker.feet() - start), facing.heading) > 0.9);

    Player runner = standing();
    REQUIRE(runner.play(stride(facing, {0.0, 0.0, 1.0}, true), 0.4));
    CHECK(apart(start, runner.feet()) > 3.0 * walked);
}

TEST_CASE("a player jumps and lands again", "[client][player]") {
    Player player = standing();
    const double ground = height_of(player.feet());
    REQUIRE(player.play(stride(aim(player), {0.0, 1.0, 0.0}, false), 0.2));
    CHECK(height_of(player.feet()) > ground + 0.4);
    CHECK_FALSE(player.standing());
    REQUIRE(player.play({}, 2.0));
    CHECK(player.standing());
    CHECK(height_of(player.feet()) == Approx(ground).margin(0.05));
}

TEST_CASE("a player is put where the server says", "[client][player]") {
    Player player = standing();
    const WorldPosition there = player.feet() + (aim(player).heading * 3.0);
    player.place(there);
    CHECK(glm::length(player.feet() - there) < 1e-6);
}

TEST_CASE("the camera keeps off the ground behind the player", "[client][player]") {
    const Player player = standing();
    const WorldPosition shoulder = player.shoulder();
    const DVec3 up = glm::normalize(shoulder.metres());
    CHECK(glm::length(shoulder - player.feet()) == Approx(PlaySettings{}.shoulder));
    // Nothing in the way above, the ground below.
    CHECK(player.room(shoulder, up, 4.0) == 4.0);
    // A little aslant, as rays straight down a vertex of the ground can slip between its
    // triangles.
    const DVec3 down = glm::normalize(aim(player).heading - (up * 5.0));
    // Were the ground level; it dips or rises a little behind.
    const double to_ground = PlaySettings{}.shoulder / -glm::dot(down, up);
    CHECK(player.room(shoulder, down, 4.0) ==
          Approx(to_ground - PlaySettings{}.clearance).margin(0.5));
}

TEST_CASE("the keys stride the way the aim faces, jumping with up", "[client][player]") {
    const Flight facing = start_flight({1.0, 0.0, 0.0}, 1'000.0, 0.0, {0.0, 1.0, 0.0});
    const physics::Stride ahead = stride(facing, {0.0, 0.0, 1.0}, false);
    CHECK(ahead.along.y == Approx(1.0));
    CHECK_FALSE(ahead.running);
    CHECK_FALSE(ahead.jumping);
    // Facing +y with +x up, right is -z.
    const physics::Stride aside = stride(facing, {1.0, 1.0, 0.0}, true);
    CHECK(aside.along.z == Approx(-1.0));
    CHECK(aside.running);
    CHECK(aside.jumping);
}

TEST_CASE("a player placed by the server flies there, or stands on the ground under it",
          "[client][player]") {
    const double ground = ground_at(on_land());
    const protocol::Facing facing{.heading = 40'000, .pitch = -1'000};
    const auto placed_at = [&](double height, protocol::Stance stance) {
        auto placed =
            placing(client::testing::globe().world,
                    {.at = protocol::to_millimetres(WorldPosition{on_land() * (radius() + height)}),
                     .stance = stance,
                     .facing = facing});
        REQUIRE(placed);
        CHECK(facing_of(placed->flight) == facing);
        return std::move(placed).value();
    };

    const Placing flying = placed_at(ground + 300.0, protocol::Stance::Flying);
    CHECK_FALSE(flying.player);
    CHECK(height_of(flying.flight.eye) == Approx(ground + 300.0).margin(0.01));

    // Where the ground has sunk or risen since.
    for (const double height : {ground + 40.0, ground - 10.0}) {
        const Placing walking = placed_at(height, protocol::Stance::Walking);
        const Player* const player = walking.player ? &*walking.player : nullptr;
        REQUIRE(player != nullptr);
        const WorldPosition feet = player->feet();
        CHECK(height_of(feet) == Approx(ground).margin(2.0));
        CHECK(apart(feet, WorldPosition{on_land() * radius()}) < 0.01);
    }
}

TEST_CASE("the ground is settled under a place once its tiles are as fine as they'll be sent",
          "[client][player]") {
    const globe::Generator& generator = client::testing::globe().generator;
    const globe::Layers& layers = generator.layers();
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    // An odd tile up, so the one over it is the first of a coarser pair.
    const auto up = static_cast<std::uint32_t>(layers.count) / side / 2;
    const std::uint32_t k = up % 2 == 1 ? up : up - 1;
    const globe::TileKey ground{.level = 0, .face = 2, .i = 10, .j = 10, .k = k};
    const DVec3 at = globe::center(globe::Cell{.level = static_cast<std::uint8_t>(layers.level),
                                               .face = 2,
                                               .i = (10 * side) + 5,
                                               .j = (10 * side) + 5}) *
                     layers.bottom(static_cast<int>((k + 1) * side));
    const auto filled_to = [](int height) {
        return std::make_shared<const globe::VoxelTree>(
            globe::VoxelTree::build(globe::Volume::tile_side, [&](int, int, int z) {
                return z < height ? globe::Cube{.solid = globe::Material::Granite,
                                                .solid_amount = globe::Voxel::full}
                                  : globe::Cube{};
            }));
    };
    globe::HeldTiles tiles;
    globe::Volume none;
    globe::World world{generator.planet(), layers, none};
    world.read_tiles(&tiles);
    CHECK_FALSE(ground_settled(world, tiles, at));
    tiles.put(ground, filled_to(20));
    CHECK_FALSE(ground_settled(world, tiles, at));
    // Over it, ground still to come finer; then open air, sent whole.
    const globe::TileKey over =
        globe::TileKey{.level = 0, .face = 2, .i = 10, .j = 10, .k = k + 1}.parent();
    tiles.put(over, filled_to(1));
    CHECK_FALSE(ground_settled(world, tiles, at));
    tiles.put(over, filled_to(0));
    CHECK(ground_settled(world, tiles, at));
    // Its own tile held only coarser, finer is still to come.
    tiles.put(ground.parent(), filled_to(20));
    CHECK_FALSE(ground_settled(world, tiles, at));
}
