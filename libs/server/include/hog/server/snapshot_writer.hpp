#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/save/snapshot.hpp>
#include <hog/save/snapshot_store.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace hog::server {

// Says when a periodic snapshot is due, in game time.
class SaveTimer {
public:
    // The first snapshot is due one interval after `start`. Precondition: interval > 0.
    SaveTimer(GameTime start, GameDuration interval);

    // True once the interval has passed, then counts the next interval from `now`, so a burst of
    // fast ticks asks for one snapshot rather than many.
    [[nodiscard]] bool due(GameTime now);

private:
    GameDuration interval_;
    GameTime next_;
};

// Encodes and writes snapshots on its own thread, so a save never stalls a tick. A snapshot
// submitted while another is waiting replaces it: only the newest state matters.
class SnapshotWriter {
public:
    struct Stats {
        std::uint64_t written = 0;
        // Replaced by a newer snapshot before they were written.
        std::uint64_t replaced = 0;
        std::uint64_t failed = 0;
    };

    // The store must outlive the writer and be used by nothing else meanwhile. Precondition:
    // keep > 0.
    SnapshotWriter(save::SnapshotStore& store, std::size_t keep);
    // Writes whatever is waiting first.
    ~SnapshotWriter();
    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter(SnapshotWriter&&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(SnapshotWriter&&) = delete;

    void submit(save::Snapshot snapshot);

    // Waits until everything submitted so far is written, then returns the first failure since
    // the last flush, if any.
    Result<void> flush();

    [[nodiscard]] Stats stats() const;

private:
    void run();

    save::SnapshotStore* store_;
    std::size_t keep_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::optional<save::Snapshot> waiting_;
    bool writing_ = false;
    bool stopping_ = false;
    std::optional<Error> failure_;
    Stats stats_;
    // Last, so it starts after everything it uses exists.
    std::thread thread_;
};

} // namespace hog::server
