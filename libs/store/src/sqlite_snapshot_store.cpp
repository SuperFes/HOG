#include "sqlite.hpp"

#include <hog/store/sqlite_snapshot_store.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <utility>

namespace hog::store {

using sqlite::failure;
using sqlite::Statement;

SqliteSnapshotStore::SqliteSnapshotStore(Database database) : database_{std::move(database)} {}

Result<SqliteSnapshotStore> SqliteSnapshotStore::open(const std::filesystem::path& path) {
    auto database = open_database(path);
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    return SqliteSnapshotStore{*std::move(database)};
}

Result<SqliteSnapshotStore::Id> SqliteSnapshotStore::write(GameTime time,
                                                           std::span<const std::byte> encoded) {
    sqlite3* database = database_.get();
    Statement insert{database, "INSERT INTO snapshots (time, data) VALUES (?, ?)"};
    if (!insert.ok()) {
        return failure(database, "preparing a snapshot write");
    }
    sqlite3_bind_int64(insert.get(), 1, time.time_since_epoch().count());
    // An empty span's data may be null, which SQLite would store as NULL.
    const void* bytes = encoded.empty() ? static_cast<const void*>("") : encoded.data();
    sqlite3_bind_blob64(insert.get(), 2, bytes, encoded.size(), SQLITE_STATIC);
    if (insert.step() != SQLITE_DONE) {
        return failure(database, "writing a snapshot");
    }
    return sqlite3_last_insert_rowid(database);
}

Result<std::vector<SqliteSnapshotStore::Id>> SqliteSnapshotStore::list() const {
    sqlite3* database = database_.get();
    Statement select{database, "SELECT id FROM snapshots ORDER BY id DESC"};
    if (!select.ok()) {
        return failure(database, "preparing a snapshot listing");
    }
    std::vector<Id> ids;
    int stepped = SQLITE_ROW;
    while ((stepped = select.step()) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(select.get(), 0));
    }
    if (stepped != SQLITE_DONE) {
        return failure(database, "listing snapshots");
    }
    return ids;
}

Result<std::vector<std::byte>> SqliteSnapshotStore::read(Id id) const {
    sqlite3* database = database_.get();
    Statement select{database, "SELECT data FROM snapshots WHERE id = ?"};
    if (!select.ok()) {
        return failure(database, "preparing a snapshot read");
    }
    sqlite3_bind_int64(select.get(), 1, id);
    const int stepped = select.step();
    if (stepped == SQLITE_DONE) {
        return make_error(ErrorCode::NotFound, std::format("no snapshot {}", id));
    }
    if (stepped != SQLITE_ROW) {
        return failure(database, "reading a snapshot");
    }
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(select.get(), 0));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(select.get(), 0));
    const std::span<const std::byte> blob{data, size};
    return std::vector<std::byte>(blob.begin(), blob.end());
}

Result<std::size_t> SqliteSnapshotStore::prune(std::size_t keep) {
    sqlite3* database = database_.get();
    Statement erase{database, "DELETE FROM snapshots WHERE id NOT IN "
                              "(SELECT id FROM snapshots ORDER BY id DESC LIMIT ?)"};
    if (!erase.ok()) {
        return failure(database, "preparing a snapshot prune");
    }
    const auto limit = std::min<std::size_t>(keep, std::numeric_limits<std::int64_t>::max());
    sqlite3_bind_int64(erase.get(), 1, static_cast<std::int64_t>(limit));
    if (erase.step() != SQLITE_DONE) {
        return failure(database, "pruning snapshots");
    }
    return static_cast<std::size_t>(sqlite3_changes64(database));
}

} // namespace hog::store
