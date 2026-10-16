#include <hog/replication/replica.hpp>
#include <hog/replication/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

using hog::ErrorCode;
using namespace hog::protocol;
using namespace hog::replication;

namespace {

Snapshot whole(std::uint32_t tick, std::vector<Entered> entered) {
    return {.tick = tick, .baseline = {}, .entered = std::move(entered), .changed = {}, .left = {}};
}

Snapshot moving(std::uint32_t tick, std::uint32_t baseline, Millimetres by) {
    return {.tick = tick,
            .baseline = baseline,
            .entered = {},
            .changed = {{.id = NetId{1}, .moved_by = by}},
            .left = {}};
}

Entered matt() {
    return {.id = NetId{1}, .at = Millimetres{100, 0, 0}, .name = "Matt"};
}

} // namespace

TEST_CASE("a replica holds nothing until a snapshot comes", "[replication][replica]") {
    const Replica replica;
    CHECK(replica.now().empty());
    CHECK_FALSE(replica.tick());
}

TEST_CASE("a replica builds the world from deltas on what it holds", "[replication][replica]") {
    Replica replica;
    CHECK(replica.receive(whole(1, {matt()})) == true);
    CHECK(replica.receive(moving(2, 1, Millimetres{5, 0, 0})) == true);
    // Coded against 1 again, as the server hadn't heard 2 was received.
    CHECK(replica.receive(moving(3, 1, Millimetres{8, 0, 0})) == true);
    CHECK(replica.tick() == 3);
    REQUIRE(replica.now().size() == 1);
    CHECK(replica.now().front().at == Millimetres{108, 0, 0});
    CHECK(replica.now().front().name == "Matt");
}

TEST_CASE("a replica ignores snapshots older than it holds", "[replication][replica]") {
    Replica replica;
    REQUIRE(replica.receive(whole(1, {matt()})) == true);
    REQUIRE(replica.receive(moving(3, 1, Millimetres{8, 0, 0})) == true);
    CHECK(replica.receive(moving(2, 1, Millimetres{5, 0, 0})) == false);
    CHECK(replica.receive(moving(3, 1, Millimetres{5, 0, 0})) == false);
    CHECK(replica.tick() == 3);
    CHECK(replica.now().front().at == Millimetres{108, 0, 0});
}

TEST_CASE("a replica ignores snapshots coded against what it doesn't hold",
          "[replication][replica]") {
    Replica replica;
    REQUIRE(replica.receive(whole(1, {matt()})) == true);
    CHECK(replica.receive(moving(4, 2, Millimetres{5, 0, 0})) == false);
    for (std::uint32_t tick = 2; tick < 2 + Replica::kept; ++tick) {
        REQUIRE(replica.receive(moving(tick, tick - 1, Millimetres{1, 0, 0})) == true);
    }
    // Snapshot 1 is no longer kept.
    CHECK(replica.receive(moving(100, 1, Millimetres{5, 0, 0})) == false);
}

TEST_CASE("a snapshot that doesn't fit its baseline fails", "[replication][replica]") {
    Replica replica;
    REQUIRE(replica.receive(whole(1, {})) == true);
    const auto broken = replica.receive(moving(2, 1, Millimetres{1, 0, 0}));
    REQUIRE_FALSE(broken);
    CHECK(broken.error().code == ErrorCode::NotFound);
    CHECK(replica.tick() == 1);
}

TEST_CASE("a replica keeps up with a view through loss and reordering", "[replication][replica]") {
    std::mt19937 random{88};
    std::bernoulli_distribution lost{0.3};
    std::uniform_int_distribution<int> step{-400, 400};
    View view{{.radius = 100.0, .leave_at = 1.1, .budget = 1'200}};
    Replica replica;

    // Entities wander near the client, now and then out of view and back.
    std::vector<Nearby> nearby;
    for (std::uint32_t id = 1; id <= 30; ++id) {
        nearby.push_back({.id = NetId{id},
                          .at = Millimetres{id * 1'000, 0, 0},
                          .name = "someone",
                          .distance = id * 3.0});
    }
    std::vector<Snapshot> late;
    for (int tick = 0; tick < 600; ++tick) {
        for (Nearby& entity : nearby) {
            entity.at += Millimetres{step(random), step(random), 0};
            entity.distance = std::clamp(entity.distance + (step(random) / 100.0), 0.0, 120.0);
        }
        const Snapshot snapshot = view.next(nearby);
        // Some are lost, some come a tick late, after the next.
        if (lost(random)) {
            continue;
        }
        late.push_back(snapshot);
        if (late.size() < 2 && lost(random)) {
            continue;
        }
        std::ranges::shuffle(late, random);
        for (const Snapshot& arrived : late) {
            const auto taken = replica.receive(arrived);
            REQUIRE(taken);
            if (*taken) {
                view.acknowledged(arrived.tick);
            }
        }
        late.clear();
    }
    // Once nothing's lost, the client holds what the view sent.
    for (int tick = 0; tick < 3; ++tick) {
        const Snapshot snapshot = view.next(nearby);
        REQUIRE(replica.receive(snapshot) == true);
        view.acknowledged(snapshot.tick);
    }
    CHECK(replica.now() == view.sent());
    for (const EntityState& entity : replica.now()) {
        const auto truth = std::ranges::find(nearby, entity.id, &Nearby::id);
        REQUIRE(truth != nearby.end());
        CHECK(entity.at == truth->at);
    }
}
