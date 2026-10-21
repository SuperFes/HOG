#include <hog/replication/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace hog::protocol;
using namespace hog::replication;

namespace {

constexpr ViewSettings settings{.radius = 100.0, .leave_at = 1.1, .budget = 1'200};

// An entity `distance` metres away, along x.
Nearby at(std::uint32_t id, double distance) {
    return {.id = NetId{id},
            .at = Millimetres{static_cast<std::int64_t>(distance * 1'000.0), 0, 0},
            .name = "someone",
            .distance = distance};
}

std::vector<NetId> ids(const EntityStates& states) {
    std::vector<NetId> out;
    out.reserve(states.size());
    for (const EntityState& state : states) {
        out.push_back(state.id);
    }
    return out;
}

std::vector<NetId> ids(const std::vector<Entered>& entered) {
    std::vector<NetId> out;
    out.reserve(entered.size());
    for (const Entered& entity : entered) {
        out.push_back(entity.id);
    }
    return out;
}

// What a client holds after applying `snapshot` to `held`, which it acknowledges.
EntityStates receive(View& view, const EntityStates& held, const Snapshot& snapshot) {
    auto states = hog::replication::apply(held, snapshot);
    REQUIRE(states);
    view.acknowledged(snapshot.tick);
    return *states;
}

} // namespace

TEST_CASE("a first snapshot is whole, of what's within the radius", "[replication][view]") {
    View view{settings};
    const std::vector nearby{at(3, 105.0), at(1, 50.0), at(2, 99.0)};
    const Snapshot snapshot = view.next(nearby);
    CHECK(snapshot.tick == 1);
    CHECK_FALSE(snapshot.baseline);
    CHECK(ids(snapshot.entered) == std::vector{NetId{1}, NetId{2}});
    CHECK(snapshot.entered.front().name == "someone");
    CHECK(snapshot.changed.empty());
    CHECK(snapshot.left.empty());
    CHECK(ids(view.sent()) == std::vector{NetId{1}, NetId{2}});
}

TEST_CASE("until one is acknowledged, each snapshot is whole", "[replication][view]") {
    View view{settings};
    const std::vector nearby{at(1, 50.0)};
    (void)view.next(nearby);
    const Snapshot again = view.next(nearby);
    CHECK(again.tick == 2);
    CHECK_FALSE(again.baseline);
    CHECK(ids(again.entered) == std::vector{NetId{1}});
}

TEST_CASE("snapshots are coded against the newest acknowledged", "[replication][view]") {
    View view{settings};
    EntityStates held = receive(view, {}, view.next(std::vector{at(1, 50.0), at(2, 60.0)}));

    // 1 stays put, 2 moves, 3 comes, and 4 is too far to.
    const std::vector nearby{at(1, 50.0), at(2, 61.5), at(3, 10.0), at(4, 104.0)};
    const Snapshot snapshot = view.next(nearby);
    CHECK(snapshot.baseline == 1);
    CHECK(ids(snapshot.entered) == std::vector{NetId{3}});
    REQUIRE(snapshot.changed.size() == 1);
    CHECK(snapshot.changed.front().id == NetId{2});
    CHECK(snapshot.changed.front().moved_by == Millimetres{1'500, 0, 0});
    CHECK(snapshot.left.empty());
    held = receive(view, held, snapshot);
    CHECK(held == view.sent());

    // 1 goes out of reach.
    const Snapshot later = view.next(std::vector{at(2, 61.5), at(3, 10.0)});
    CHECK(later.baseline == 2);
    CHECK(later.left == std::vector{NetId{1}});
    CHECK(later.entered.empty());
    CHECK(later.changed.empty());
    CHECK(receive(view, held, later) == view.sent());
}

TEST_CASE("an entity in view leaves only past the leaving distance", "[replication][view]") {
    View view{settings};
    EntityStates held = receive(view, {}, view.next(std::vector{at(1, 95.0)}));
    const Snapshot edge = view.next(std::vector{at(1, 108.0)});
    CHECK(edge.left.empty());
    CHECK(edge.changed.size() == 1);
    held = receive(view, held, edge);

    const Snapshot gone = view.next(std::vector{at(1, 112.0)});
    CHECK(gone.left == std::vector{NetId{1}});
    held = receive(view, held, gone);
    // Nor does it come back until it's within the radius again.
    CHECK(view.next(std::vector{at(1, 105.0)}).entered.empty());
}

TEST_CASE("snapshots keep to the budget, nearest first, the rest later", "[replication][view]") {
    View view{{.radius = 100.0, .leave_at = 1.1, .budget = 120}};
    std::vector<Nearby> nearby;
    for (std::uint32_t id = 1; id <= 20; ++id) {
        nearby.push_back(at(id, 100.0 - id));
    }
    EntityStates held;
    std::size_t first = 0;
    for (int round = 0; round < 20 && held.size() < nearby.size(); ++round) {
        const Snapshot snapshot = view.next(nearby);
        CHECK(encode(snapshot).size() <= 120);
        held = receive(view, held, snapshot);
        if (round == 0) {
            first = held.size();
            // The nearest have the largest ids.
            CHECK(snapshot.entered.back().id == NetId{20});
        }
    }
    CHECK(first > 0);
    CHECK(first < nearby.size());
    CHECK(held.size() == nearby.size());
}

TEST_CASE("an entity's change that doesn't fit stays as the client has it", "[replication][view]") {
    // Room for both to enter, at about 18 bytes each, but for only one far move of 21.
    View view{{.radius = 100.0, .leave_at = 1.1, .budget = 64}};
    EntityStates held = receive(view, {}, view.next(std::vector{at(1, 10.0), at(2, 20.0)}));
    REQUIRE(held.size() == 2);
    std::vector moved{at(1, 10.0), at(2, 20.0)};
    for (Nearby& entity : moved) {
        entity.at += Millimetres{1'000'000'000};
    }
    const Snapshot snapshot = view.next(moved);
    REQUIRE(snapshot.changed.size() == 1);
    CHECK(snapshot.changed.front().id == NetId{1});
    held = receive(view, held, snapshot);
    CHECK(held == view.sent());

    const Snapshot rest = view.next(moved);
    REQUIRE(rest.changed.size() == 1);
    CHECK(rest.changed.front().id == NetId{2});
    CHECK(receive(view, held, rest) == view.sent());
}

TEST_CASE("acks too old to code against give a whole snapshot", "[replication][view]") {
    View view{settings};
    const std::vector nearby{at(1, 50.0)};
    (void)view.next(nearby);
    view.acknowledged(1);
    for (std::size_t sent = 0; sent < View::kept; ++sent) {
        (void)view.next(nearby);
    }
    const Snapshot snapshot = view.next(nearby);
    CHECK_FALSE(snapshot.baseline);
    CHECK(ids(snapshot.entered) == std::vector{NetId{1}});
}

TEST_CASE("acks of snapshots not sent, or older, are ignored", "[replication][view]") {
    View view{settings};
    const std::vector nearby{at(1, 50.0)};
    (void)view.next(nearby);
    (void)view.next(nearby);
    view.acknowledged(9);
    view.acknowledged(0);
    CHECK_FALSE(view.next(nearby).baseline);
    view.acknowledged(2);
    view.acknowledged(1);
    CHECK(view.next(nearby).baseline == 2);
}
