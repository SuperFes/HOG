#include <hog/planet/projection.hpp>
#include <hog/server/players.hpp>
#include <hog/server/spatial_tracking.hpp>

#include <catch2/catch_test_macros.hpp>
#include <flecs.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::net::ConnectionId;
using hog::protocol::NetId;
using hog::protocol::Snapshot;
using hog::server::Players;
using hog::server::Position;
using hog::server::Replicated;

namespace {

constexpr double radius = 115'000.0;

WorldPosition at(double metres) {
    return WorldPosition{DVec3{0.0, 0.0, metres}};
}

// On the surface, `metres` along it from the first player's place.
WorldPosition along(double metres) {
    return WorldPosition{DVec3{metres, 0.0, radius}};
}

// The players and their world, with positions indexed as the server keeps them.
struct Shared {
    Shared() { hog::server::track_positions(world, index); }

    // Each placed player's next snapshot.
    std::map<ConnectionId, Snapshot> share() {
        std::map<ConnectionId, Snapshot> sent;
        players.share(index, [&](ConnectionId connection, const Snapshot& snapshot) {
            sent.emplace(connection, snapshot);
        });
        return sent;
    }

    // Before the world, which it must outlive.
    hog::server::SpatialIndex index{hog::planet::level_for(500.0, radius)};
    flecs::world world;
    Players players{world, radius};
};

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

TEST_CASE("players are shown the others near them, but not themselves", "[server][players]") {
    Shared shared;
    Players& players = shared.players;
    const flecs::entity matt = players.join(ConnectionId{7}, "Matt");
    const flecs::entity anna = players.join(ConnectionId{8}, "Anna");
    players.join(ConnectionId{9}, "Kim");
    players.join(ConnectionId{10}, "Lee");
    CHECK(shared.share().empty());

    REQUIRE(players.move(ConnectionId{7}, along(0.0)));
    REQUIRE(players.move(ConnectionId{8}, along(100.0)));
    REQUIRE(players.move(ConnectionId{10}, along(5'000.0)));
    // Placed, but not one that's replicated.
    shared.world.entity().set<Position>({along(10.0)});
    auto sent = shared.share();
    // Kim, with no place yet, is sent nothing.
    REQUIRE(sent.size() == 3);
    const Snapshot& to_matt = sent.at(ConnectionId{7});
    CHECK(to_matt.tick == 1);
    CHECK_FALSE(to_matt.baseline);
    REQUIRE(to_matt.entered.size() == 1);
    CHECK(to_matt.entered[0].id == anna.get<Replicated>().id);
    CHECK(to_matt.entered[0].name == "Anna");
    CHECK(to_matt.entered[0].at == hog::protocol::to_millimetres(along(100.0)));
    REQUIRE(sent.at(ConnectionId{8}).entered.size() == 1);
    CHECK(sent.at(ConnectionId{8}).entered[0].id == matt.get<Replicated>().id);
    CHECK(sent.at(ConnectionId{10}).entered.empty());
}

TEST_CASE("players are sent changes since what they acknowledged", "[server][players]") {
    Shared shared;
    Players& players = shared.players;
    players.join(ConnectionId{7}, "Matt");
    const NetId anna = players.join(ConnectionId{8}, "Anna").get<Replicated>().id;
    REQUIRE(players.move(ConnectionId{7}, along(0.0)));
    REQUIRE(players.move(ConnectionId{8}, along(100.0)));
    const std::uint32_t first = shared.share().at(ConnectionId{7}).tick;
    players.acknowledged(ConnectionId{7}, first);
    // Ignored for those who haven't joined.
    players.acknowledged(ConnectionId{99}, first);

    REQUIRE(players.move(ConnectionId{8}, along(101.0)));
    const Snapshot moved = shared.share().at(ConnectionId{7});
    CHECK(moved.baseline == first);
    CHECK(moved.entered.empty());
    REQUIRE(moved.changed.size() == 1);
    CHECK(moved.changed[0].id == anna);
    CHECK(moved.changed[0].moved_by == hog::protocol::Millimetres{1'000, 0, 0});

    players.acknowledged(ConnectionId{7}, moved.tick);
    players.leave(ConnectionId{8});
    const Snapshot gone = shared.share().at(ConnectionId{7});
    CHECK(gone.baseline == moved.tick);
    CHECK(gone.left == std::vector{anna});
}
