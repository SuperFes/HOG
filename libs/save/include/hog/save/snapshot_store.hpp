#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/save/snapshot.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::save {

// Durable storage for encoded snapshots, kept in the order written. Backends are chosen at
// runtime, so this is an interface.
class SnapshotStore {
public:
    // Increases with every write and is never reused.
    using Id = std::int64_t;

    SnapshotStore() = default;
    virtual ~SnapshotStore() = default;

    // `time` is the snapshot's game time, kept so stored snapshots can be listed without decoding.
    virtual Result<Id> write(GameTime time, std::span<const std::byte> encoded) = 0;

    // Stored snapshots, newest first.
    [[nodiscard]] virtual Result<std::vector<Id>> list() const = 0;

    // Fails with NotFound for an id that isn't stored.
    [[nodiscard]] virtual Result<std::vector<std::byte>> read(Id id) const = 0;

    // Removes all but the newest `keep`; returns how many were removed.
    virtual Result<std::size_t> prune(std::size_t keep) = 0;

protected:
    SnapshotStore(const SnapshotStore&) = default;
    SnapshotStore(SnapshotStore&&) = default;
    SnapshotStore& operator=(const SnapshotStore&) = default;
    SnapshotStore& operator=(SnapshotStore&&) = default;
};

// Writes the snapshot, then removes all but the newest `keep`. Precondition: keep > 0.
[[nodiscard]] Result<void> save(SnapshotStore& store, const Snapshot& snapshot, std::size_t keep);

struct Loaded {
    // Empty when no stored snapshot decodes.
    std::optional<Snapshot> snapshot;
    // Why each newer snapshot was passed over.
    std::vector<Error> skipped;
};

// The newest snapshot that decodes, passing over damaged ones. Fails on a snapshot of another
// world or from a newer build, rather than fall back to older state.
[[nodiscard]] Result<Loaded> load_newest(const SnapshotStore& store, std::uint64_t world);

} // namespace hog::save
