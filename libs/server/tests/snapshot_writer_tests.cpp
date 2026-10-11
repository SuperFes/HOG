#include <hog/save/memory_snapshot_store.hpp>
#include <hog/server/snapshot_writer.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::save::MemorySnapshotStore;
using hog::save::Snapshot;
using hog::save::SnapshotStore;
using hog::server::SaveTimer;
using hog::server::SnapshotWriter;

namespace {

Snapshot at_tick(std::uint64_t tick) {
    return Snapshot{.world = 1,
                    .time = GameTime{std::chrono::seconds{static_cast<std::int64_t>(tick)}},
                    .tick = hog::Tick{tick},
                    .sections = {}};
}

// Holds every write until released, and can fail them.
class GatedStore final : public SnapshotStore {
public:
    hog::Result<Id> write(GameTime time, std::span<const std::byte> encoded) override {
        std::unique_lock lock{mutex_};
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
        if (failing_) {
            return hog::make_error(hog::ErrorCode::Io, "disk full");
        }
        return inner_.write(time, encoded);
    }
    [[nodiscard]] hog::Result<std::vector<Id>> list() const override { return inner_.list(); }
    [[nodiscard]] hog::Result<std::vector<std::byte>> read(Id id) const override {
        return inner_.read(id);
    }
    hog::Result<std::size_t> prune(std::size_t keep) override { return inner_.prune(keep); }

    // Read only once the writer has flushed or stopped.
    [[nodiscard]] std::vector<MemorySnapshotStore::Stored>& stored() { return inner_.stored(); }
    void set_failing(bool failing) { failing_ = failing; }

    void wait_for_writes(int count) {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [&] { return entered_ >= count; });
    }
    void open() {
        {
            const std::scoped_lock lock{mutex_};
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    MemorySnapshotStore inner_;
    std::atomic<bool> failing_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    int entered_ = 0;
    bool open_ = false;
};

std::uint64_t tick_of(const MemorySnapshotStore::Stored& stored) {
    const auto snapshot = hog::save::decode(stored.bytes);
    REQUIRE(snapshot);
    return snapshot->tick.value();
}

} // namespace

TEST_CASE("snapshots fall due once per interval", "[server][save]") {
    SaveTimer timer{GameTime{1h}, 5min};
    CHECK_FALSE(timer.due(GameTime{1h}));
    CHECK_FALSE(timer.due(GameTime{1h + 4min}));
    CHECK(timer.due(GameTime{1h + 5min}));
    CHECK_FALSE(timer.due(GameTime{1h + 6min}));
    // After a long catch-up, one snapshot rather than one per missed interval.
    CHECK(timer.due(GameTime{3h}));
    CHECK_FALSE(timer.due(GameTime{3h + 1min}));
    CHECK(timer.due(GameTime{3h + 5min}));

    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(SaveTimer(GameTime{}, 0s), hog::testing::AssertionFailed);
}

TEST_CASE("the writer stores what it is given", "[server][save]") {
    MemorySnapshotStore store;
    SnapshotWriter writer{store, 2};
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        writer.submit(at_tick(tick));
        REQUIRE(writer.flush());
    }
    CHECK(writer.stats().written == 3);
    REQUIRE(store.stored().size() == 2);
    CHECK(tick_of(store.stored().back()) == 3);
}

TEST_CASE("a waiting snapshot is replaced by a newer one", "[server][save]") {
    GatedStore store;
    SnapshotWriter writer{store, 5};
    writer.submit(at_tick(1));
    store.wait_for_writes(1);
    writer.submit(at_tick(2));
    writer.submit(at_tick(3));
    store.open();
    REQUIRE(writer.flush());

    CHECK(writer.stats().written == 2);
    CHECK(writer.stats().replaced == 1);
    REQUIRE(store.stored().size() == 2);
    CHECK(tick_of(store.stored()[0]) == 1);
    CHECK(tick_of(store.stored()[1]) == 3);
}

TEST_CASE("write failures are reported once", "[server][save]") {
    GatedStore store;
    store.set_failing(true);
    store.open();
    SnapshotWriter writer{store, 1};
    writer.submit(at_tick(1));
    const auto failed = writer.flush();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == hog::ErrorCode::Io);
    CHECK(writer.flush());
    CHECK(writer.stats().failed == 1);

    store.set_failing(false);
    writer.submit(at_tick(2));
    CHECK(writer.flush());
    CHECK(writer.stats().written == 1);
}

TEST_CASE("a waiting snapshot is written before the writer stops", "[server][save]") {
    GatedStore store;
    {
        SnapshotWriter writer{store, 5};
        writer.submit(at_tick(1));
        store.wait_for_writes(1);
        writer.submit(at_tick(2));
        store.open();
    }
    REQUIRE(store.stored().size() == 2);
    CHECK(tick_of(store.stored()[1]) == 2);
}

TEST_CASE("a writer needs to keep something", "[server][save]") {
    const hog::testing::AssertionGuard guard;
    MemorySnapshotStore store;
    CHECK_THROWS_AS(SnapshotWriter(store, 0), hog::testing::AssertionFailed);
}
