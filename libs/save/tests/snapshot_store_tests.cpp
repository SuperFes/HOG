#include <hog/save/memory_snapshot_store.hpp>
#include <hog/save/snapshot_store.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::ErrorCode;
using hog::save::MemorySnapshotStore;
using hog::save::Section;
using hog::save::section_id;
using hog::save::Snapshot;
using hog::save::SnapshotStore;

namespace {

constexpr std::uint64_t world = 0xC0FFEE;

Snapshot at_tick(std::uint64_t tick, std::uint64_t of_world = world) {
    Snapshot snapshot{
        .world = of_world,
        .time = hog::GameTime{std::chrono::seconds{static_cast<std::int64_t>(tick)}},
        .tick = hog::Tick{tick},
        .sections = {},
    };
    snapshot.add(Section{.id = section_id("TEST"), .version = 1, .bytes = {std::byte{7}}});
    return snapshot;
}

// Fails whichever calls it is told to.
class BrokenStore final : public SnapshotStore {
public:
    bool fail_write = false;
    bool fail_list = false;
    bool fail_read = false;
    bool fail_prune = false;
    MemorySnapshotStore inner;

    hog::Result<Id> write(hog::GameTime time, std::span<const std::byte> encoded) override {
        if (fail_write) {
            return broken();
        }
        return inner.write(time, encoded);
    }
    [[nodiscard]] hog::Result<std::vector<Id>> list() const override {
        if (fail_list) {
            return broken();
        }
        return inner.list();
    }
    [[nodiscard]] hog::Result<std::vector<std::byte>> read(Id id) const override {
        if (fail_read) {
            return broken();
        }
        return inner.read(id);
    }
    hog::Result<std::size_t> prune(std::size_t keep) override {
        if (fail_prune) {
            return broken();
        }
        return inner.prune(keep);
    }

private:
    [[nodiscard]] static std::unexpected<hog::Error> broken() {
        return hog::make_error(ErrorCode::Io, "disk on fire");
    }
};

} // namespace

TEST_CASE("the memory store keeps snapshots in order", "[save][store]") {
    MemorySnapshotStore store;
    CHECK(store.list() == std::vector<SnapshotStore::Id>{});
    const auto first = store.write(hog::GameTime{}, std::vector{std::byte{1}});
    const auto second = store.write(hog::GameTime{}, std::vector{std::byte{2}});
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*second > *first);
    CHECK(store.list() == std::vector{*second, *first});
    CHECK(store.read(*first) == std::vector{std::byte{1}});

    CHECK(store.prune(5) == 0U);
    CHECK(store.prune(1) == 1U);
    CHECK(store.list() == std::vector{*second});
    const auto missing = store.read(*first);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    // Ids are never reused, even once pruned.
    CHECK(store.prune(0) == 1U);
    const auto third = store.write(hog::GameTime{}, {});
    REQUIRE(third);
    CHECK(*third > *second);
}

TEST_CASE("saving keeps only the newest snapshots", "[save][store]") {
    MemorySnapshotStore store;
    for (std::uint64_t tick = 1; tick <= 5; ++tick) {
        REQUIRE(hog::save::save(store, at_tick(tick), 3));
    }
    CHECK(store.stored().size() == 3);
    CHECK(store.stored().front().time == at_tick(3).time);

    const auto loaded = hog::save::load_newest(store, world);
    REQUIRE(loaded);
    CHECK(loaded->snapshot == at_tick(5));
    CHECK(loaded->skipped.empty());

    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(static_cast<void>(hog::save::save(store, at_tick(6), 0)),
                    hog::testing::AssertionFailed);
}

TEST_CASE("loading an empty store finds nothing", "[save][store]") {
    const MemorySnapshotStore store;
    const auto loaded = hog::save::load_newest(store, world);
    REQUIRE(loaded);
    CHECK_FALSE(loaded->snapshot);
    CHECK(loaded->skipped.empty());
}

TEST_CASE("damaged snapshots are passed over for older ones", "[save][store]") {
    MemorySnapshotStore store;
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        REQUIRE(hog::save::save(store, at_tick(tick), 3));
    }
    store.stored()[2].bytes.back() ^= std::byte{1};
    store.stored()[1].bytes.resize(5);

    const auto loaded = hog::save::load_newest(store, world);
    REQUIRE(loaded);
    CHECK(loaded->snapshot == at_tick(1));
    REQUIRE(loaded->skipped.size() == 2);
    CHECK(loaded->skipped[0].message.starts_with("snapshot 3: "));
    CHECK(loaded->skipped[1].message.starts_with("snapshot 2: "));

    store.stored()[0].bytes.clear();
    const auto none = hog::save::load_newest(store, world);
    REQUIRE(none);
    CHECK_FALSE(none->snapshot);
    CHECK(none->skipped.size() == 3);
}

TEST_CASE("loading refuses other worlds and newer builds", "[save][store]") {
    MemorySnapshotStore store;
    REQUIRE(hog::save::save(store, at_tick(1), 3));
    REQUIRE(hog::save::save(store, at_tick(2, world + 1), 3));
    const auto other_world = hog::save::load_newest(store, world);
    REQUIRE_FALSE(other_world);
    CHECK(other_world.error().code == ErrorCode::InvalidArgument);

    REQUIRE(hog::save::save(store, at_tick(3), 3));
    // The container version follows the 8-byte magic.
    store.stored().back().bytes[8] = std::byte{hog::save::format_version + 1};
    const auto newer = hog::save::load_newest(store, world);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::Unsupported);
}

TEST_CASE("store failures are passed on", "[save][store]") {
    BrokenStore store;
    store.fail_write = true;
    CHECK_FALSE(hog::save::save(store, at_tick(1), 1));
    store.fail_write = false;

    store.fail_prune = true;
    CHECK_FALSE(hog::save::save(store, at_tick(1), 1));
    store.fail_prune = false;
    REQUIRE(hog::save::save(store, at_tick(2), 1));

    store.fail_list = true;
    CHECK_FALSE(hog::save::load_newest(store, world));
    store.fail_list = false;

    store.fail_read = true;
    const auto unread = hog::save::load_newest(store, world);
    REQUIRE_FALSE(unread);
    CHECK(unread.error().code == ErrorCode::Io);
}
