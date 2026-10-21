#include <hog/replication/states.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::ErrorCode;
using namespace hog::protocol;
using namespace hog::replication;

namespace {

EntityStates two() {
    return {{.id = NetId{1}, .at = Millimetres{10, 0, 0}, .name = "Matt"},
            {.id = NetId{5}, .at = Millimetres{0, 20, 0}, .name = "Anna"}};
}

Snapshot empty() {
    return {.tick = 2, .baseline = 1, .entered = {}, .changed = {}, .left = {}};
}

} // namespace

TEST_CASE("a snapshot moves, adds and drops entities from its baseline", "[replication]") {
    Snapshot snapshot = empty();
    snapshot.entered = {{.id = NetId{3}, .at = Millimetres{7, 7, 7}, .name = "Ben"}};
    snapshot.changed = {{.id = NetId{1}, .moved_by = Millimetres{-4, 2, 0}}};
    snapshot.left = {NetId{5}};
    const auto states = hog::replication::apply(two(), snapshot);
    REQUIRE(states);
    CHECK(*states == EntityStates{{.id = NetId{1}, .at = Millimetres{6, 2, 0}, .name = "Matt"},
                                  {.id = NetId{3}, .at = Millimetres{7, 7, 7}, .name = "Ben"}});
}

TEST_CASE("an empty snapshot leaves the baseline as it is", "[replication]") {
    CHECK(hog::replication::apply(two(), empty()) == two());
    CHECK(hog::replication::apply({}, empty()) == EntityStates{});
}

TEST_CASE("a snapshot that doesn't fit its baseline fails", "[replication]") {
    Snapshot changed = empty();
    changed.changed = {{.id = NetId{2}, .moved_by = {}}};
    CHECK(hog::replication::apply(two(), changed).error().code == ErrorCode::NotFound);

    Snapshot left = empty();
    left.left = {NetId{2}};
    CHECK(hog::replication::apply(two(), left).error().code == ErrorCode::NotFound);

    Snapshot entered = empty();
    entered.entered = {{.id = NetId{5}, .at = Millimetres{0}, .name = ""}};
    CHECK(hog::replication::apply(two(), entered).error().code == ErrorCode::InvalidArgument);
}
