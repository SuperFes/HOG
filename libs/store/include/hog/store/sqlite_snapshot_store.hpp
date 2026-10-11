#pragma once

#include <hog/core/result.hpp>
#include <hog/save/snapshot_store.hpp>
#include <hog/store/database.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace hog::store {

// Snapshots kept in a world's SQLite file, alongside its changes.
class SqliteSnapshotStore final : public save::SnapshotStore {
public:
    // Opens the database at `path`, creating it if missing. The path ":memory:" opens a private
    // database that lasts as long as the store.
    [[nodiscard]] static Result<SqliteSnapshotStore> open(const std::filesystem::path& path);

    Result<Id> write(GameTime time, std::span<const std::byte> encoded) override;
    [[nodiscard]] Result<std::vector<Id>> list() const override;
    [[nodiscard]] Result<std::vector<std::byte>> read(Id id) const override;
    Result<std::size_t> prune(std::size_t keep) override;

private:
    explicit SqliteSnapshotStore(Database database);

    Database database_;
};

} // namespace hog::store
