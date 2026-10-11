#include <hog/core/assert.hpp>
#include <hog/save/snapshot_store.hpp>

#include <format>
#include <utility>

namespace hog::save {

Result<void> save(SnapshotStore& store, const Snapshot& snapshot, std::size_t keep) {
    HOG_ASSERT(keep > 0, "pruning to nothing would delete the snapshot just written");
    if (auto written = store.write(snapshot.time, encode(snapshot)); !written) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto pruned = store.prune(keep); !pruned) {
        return std::unexpected(std::move(pruned.error()));
    }
    return {};
}

Result<Loaded> load_newest(const SnapshotStore& store, std::uint64_t world) {
    auto ids = store.list();
    if (!ids) {
        return std::unexpected(std::move(ids.error()));
    }
    Loaded loaded;
    for (const SnapshotStore::Id id : *ids) {
        auto bytes = store.read(id);
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        auto snapshot = decode(*bytes);
        if (!snapshot) {
            if (snapshot.error().code != ErrorCode::Parse) {
                return std::unexpected(std::move(snapshot.error()));
            }
            loaded.skipped.push_back(Error{
                .code = ErrorCode::Parse,
                .message = std::format("snapshot {}: {}", id, snapshot.error().message),
            });
            continue;
        }
        if (snapshot->world != world) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("snapshot {} is of world {:#018x}, not {:#018x}", id,
                                          snapshot->world, world));
        }
        loaded.snapshot = *std::move(snapshot);
        break;
    }
    return loaded;
}

} // namespace hog::save
