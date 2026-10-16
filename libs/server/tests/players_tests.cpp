#include <hog/server/players.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <catch2/catch_test_macros.hpp>
#include <flecs.h>

#include <utility>

using hog::DVec3;
using hog::WorldPosition;
using hog::net::ConnectionId;
using hog::protocol::NetId;
using hog::server::Players;
using hog::server::Position;
using hog::server::Replicated;

namespace {

constexpr double radius = 115'000.0;

WorldPosition at(double metres) {
    return WorldPosition{DVec3{0.0, 0.0, metres}};
}

} // namespace

TEST_CASE("a player joins with a name and an id, and no place until moving", "[server][players]") {
    flecs::world world;
    Players players{world, radius};
    const flecs::entity matt = players.join(ConnectionId{7}, "Matt");
    const flecs::entity anna = players.join(ConnectionId{8}, "Anna");
    CHECK(players.size() == 2);
    CHECK(players.find(ConnectionId{7}) == matt);
    CHECK_FALSE(players.find(ConnectionId{9}));

    REQUIRE(matt.has<Replicated>());
    CHECK(matt.get<Replicated>().name == "Matt");
    CHECK(matt.get<Replicated>().id != anna.get<Replicated>().id);
    CHECK(matt.get<Replicated>().id != NetId{0});
    CHECK_FALSE(matt.has<Position>());
}

TEST_CASE("a player moves where the client says, within bounds", "[server][players]") {
    flecs::world world;
    Players players{world, radius};
    const flecs::entity matt = players.join(ConnectionId{7}, "Matt");
    CHECK(players.move(ConnectionId{7}, at(radius + 2.0)));
    CHECK(matt.get<Position>().at.metres() == at(radius + 2.0).metres());

    CHECK_FALSE(players.move(ConnectionId{7}, at(0.0)));
    CHECK_FALSE(players.move(ConnectionId{7}, at(-radius * 3.0)));
    CHECK(matt.get<Position>().at.metres() == at(radius + 2.0).metres());
    CHECK_FALSE(players.move(ConnectionId{9}, at(radius)));
}

TEST_CASE("a player who leaves is gone, and the id isn't used again", "[server][players]") {
    flecs::world world;
    Players players{world, radius};
    const flecs::entity matt = players.join(ConnectionId{7}, "Matt");
    const NetId first = matt.get<Replicated>().id;
    players.leave(ConnectionId{7});
    players.leave(ConnectionId{7});
    CHECK(players.size() == 0);
    CHECK_FALSE(matt.is_alive());

    const flecs::entity again = players.join(ConnectionId{7}, "Matt");
    CHECK(again.get<Replicated>().id != first);
}
