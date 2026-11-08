#include "sqlite.hpp"

#include <hog/store/sqlite_volume_archive.hpp>

#include <utility>

namespace hog::store {

using sqlite::bind_bytes;
using sqlite::column_bytes;
using sqlite::execute;
using sqlite::failure;
using sqlite::Statement;

namespace {

Result<void> write_locked(sqlite3* database, std::span<const std::byte> header,
                          std::span<const globe::SavedColumn> columns) {
    Statement set_header{database, "INSERT OR REPLACE INTO volume_header (id, data) VALUES (0, ?)"};
    Statement put{database,
                  "INSERT OR REPLACE INTO volume_columns (face, i, j, data) VALUES (?, ?, ?, ?)"};
    Statement put_coarse{database, "INSERT OR REPLACE INTO volume_coarse (face, i, j, level, data) "
                                   "VALUES (?, ?, ?, ?, ?)"};
    if (!set_header.ok() || !put.ok() || !put_coarse.ok()) {
        return failure(database, "preparing a volume write");
    }
    bind_bytes(set_header, 1, header);
    if (set_header.step() != SQLITE_DONE) {
        return failure(database, "writing the volume header");
    }
    for (const globe::SavedColumn& column : columns) {
        Statement& into = column.level == 0 ? put : put_coarse;
        into.reset();
        sqlite3_bind_int(into.get(), 1, column.face);
        sqlite3_bind_int64(into.get(), 2, column.i);
        sqlite3_bind_int64(into.get(), 3, column.j);
        int next = 4;
        if (column.level > 0) {
            sqlite3_bind_int(into.get(), next++, column.level);
        }
        bind_bytes(into, next, column.bytes);
        if (into.step() != SQLITE_DONE) {
            return failure(database, "writing a tile column");
        }
    }
    return {};
}

Result<void> write_tiles_locked(sqlite3* database, std::span<const std::byte> header,
                                std::span<const globe::SavedTile> tiles) {
    Statement set_header{database, "INSERT OR REPLACE INTO volume_header (id, data) VALUES (0, ?)"};
    Statement put{database, "INSERT OR REPLACE INTO volume_tiles (level, face, i, j, k, data) "
                            "VALUES (?, ?, ?, ?, ?, ?)"};
    if (!set_header.ok() || !put.ok()) {
        return failure(database, "preparing a tile write");
    }
    bind_bytes(set_header, 1, header);
    if (set_header.step() != SQLITE_DONE) {
        return failure(database, "writing the volume header");
    }
    for (const globe::SavedTile& tile : tiles) {
        put.reset();
        sqlite3_bind_int(put.get(), 1, tile.key.level);
        sqlite3_bind_int(put.get(), 2, tile.key.face);
        sqlite3_bind_int64(put.get(), 3, tile.key.i);
        sqlite3_bind_int64(put.get(), 4, tile.key.j);
        sqlite3_bind_int64(put.get(), 5, tile.key.k);
        bind_bytes(put, 6, tile.bytes);
        if (put.step() != SQLITE_DONE) {
            return failure(database, "writing a tile");
        }
    }
    return {};
}

// All or nothing.
template <typename Write>
Result<void> in_transaction(sqlite3* database, const Write& write) {
    if (auto begun = execute(database, "BEGIN IMMEDIATE", "starting a volume write"); !begun) {
        return begun;
    }
    auto written = write();
    if (written) {
        written = execute(database, "COMMIT", "committing a volume write");
    }
    if (!written) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return written;
}

} // namespace

SqliteVolumeArchive::SqliteVolumeArchive(Database database) : database_{std::move(database)} {}

Result<SqliteVolumeArchive> SqliteVolumeArchive::open(const std::filesystem::path& path) {
    auto database = open_database(path);
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    return SqliteVolumeArchive{*std::move(database)};
}

Result<void> SqliteVolumeArchive::write(std::span<const std::byte> header,
                                        std::span<const globe::SavedColumn> columns) {
    sqlite3* database = database_.get();
    return in_transaction(database, [&] { return write_locked(database, header, columns); });
}

Result<void> SqliteVolumeArchive::write_tiles(std::span<const std::byte> header,
                                              std::span<const globe::SavedTile> tiles) {
    sqlite3* database = database_.get();
    return in_transaction(database, [&] { return write_tiles_locked(database, header, tiles); });
}

Result<std::optional<std::vector<std::byte>>> SqliteVolumeArchive::tile(globe::TileKey key) const {
    sqlite3* database = database_.get();
    Statement read{database, "SELECT data FROM volume_tiles WHERE level = ? AND face = ? AND "
                             "i = ? AND j = ? AND k = ?"};
    if (!read.ok()) {
        return failure(database, "preparing a tile read");
    }
    sqlite3_bind_int(read.get(), 1, key.level);
    sqlite3_bind_int(read.get(), 2, key.face);
    sqlite3_bind_int64(read.get(), 3, key.i);
    sqlite3_bind_int64(read.get(), 4, key.j);
    sqlite3_bind_int64(read.get(), 5, key.k);
    const int found = read.step();
    if (found == SQLITE_ROW) {
        return column_bytes(read, 0);
    }
    if (found != SQLITE_DONE) {
        return failure(database, "reading a tile");
    }
    return std::nullopt;
}

Result<std::vector<std::uint32_t>> SqliteVolumeArchive::tiles_up(globe::TileKey base) const {
    sqlite3* database = database_.get();
    Statement read{database, "SELECT k FROM volume_tiles WHERE level = ? AND face = ? AND i = ? "
                             "AND j = ? ORDER BY k"};
    if (!read.ok()) {
        return failure(database, "preparing a tile stack read");
    }
    sqlite3_bind_int(read.get(), 1, base.level);
    sqlite3_bind_int(read.get(), 2, base.face);
    sqlite3_bind_int64(read.get(), 3, base.i);
    sqlite3_bind_int64(read.get(), 4, base.j);
    std::vector<std::uint32_t> up;
    int step = read.step();
    for (; step == SQLITE_ROW; step = read.step()) {
        up.push_back(static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 0)));
    }
    if (step != SQLITE_DONE) {
        return failure(database, "reading a tile stack");
    }
    return up;
}

Result<std::vector<globe::TileKey>> SqliteVolumeArchive::tiles_at(std::uint8_t level) const {
    sqlite3* database = database_.get();
    Statement read{database, "SELECT face, i, j, k FROM volume_tiles WHERE level = ? "
                             "ORDER BY face, i, j, k"};
    if (!read.ok()) {
        return failure(database, "preparing a tiles read");
    }
    sqlite3_bind_int(read.get(), 1, level);
    std::vector<globe::TileKey> keys;
    int step = read.step();
    for (; step == SQLITE_ROW; step = read.step()) {
        keys.push_back({.level = level,
                        .face = static_cast<std::uint8_t>(sqlite3_column_int(read.get(), 0)),
                        .i = static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 1)),
                        .j = static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 2)),
                        .k = static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 3))});
    }
    if (step != SQLITE_DONE) {
        return failure(database, "reading the tiles");
    }
    return keys;
}

Result<std::vector<std::byte>> SqliteVolumeArchive::header() const {
    sqlite3* database = database_.get();
    Statement header{database, "SELECT data FROM volume_header WHERE id = 0"};
    if (!header.ok()) {
        return failure(database, "preparing a volume header read");
    }
    const int found = header.step();
    if (found == SQLITE_ROW) {
        return column_bytes(header, 0);
    }
    if (found != SQLITE_DONE) {
        return failure(database, "reading the volume header");
    }
    return std::vector<std::byte>{};
}

Result<std::optional<globe::SavedColumn>> SqliteVolumeArchive::column(std::uint8_t face,
                                                                      std::uint32_t i,
                                                                      std::uint32_t j,
                                                                      std::uint8_t level) const {
    sqlite3* database = database_.get();
    Statement read{database,
                   level == 0
                       ? "SELECT data FROM volume_columns WHERE face = ? AND i = ? AND j = ?"
                       : "SELECT data FROM volume_coarse WHERE face = ? AND i = ? AND j = ? AND "
                         "level = ?"};
    if (!read.ok()) {
        return failure(database, "preparing a tile column read");
    }
    sqlite3_bind_int(read.get(), 1, face);
    sqlite3_bind_int64(read.get(), 2, i);
    sqlite3_bind_int64(read.get(), 3, j);
    if (level > 0) {
        sqlite3_bind_int(read.get(), 4, level);
    }
    const int found = read.step();
    if (found == SQLITE_ROW) {
        return globe::SavedColumn{
            .face = face, .i = i, .j = j, .level = level, .bytes = column_bytes(read, 0)};
    }
    if (found != SQLITE_DONE) {
        return failure(database, "reading a tile column");
    }
    return std::nullopt;
}

Result<std::vector<globe::SavedColumn>> SqliteVolumeArchive::columns() const {
    sqlite3* database = database_.get();
    Statement read{database, "SELECT face, i, j, data FROM volume_columns ORDER BY face, i, j"};
    if (!read.ok()) {
        return failure(database, "preparing the tile columns read");
    }
    std::vector<globe::SavedColumn> all;
    int step = read.step();
    for (; step == SQLITE_ROW; step = read.step()) {
        all.push_back({.face = static_cast<std::uint8_t>(sqlite3_column_int(read.get(), 0)),
                       .i = static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 1)),
                       .j = static_cast<std::uint32_t>(sqlite3_column_int64(read.get(), 2)),
                       .level = 0,
                       .bytes = column_bytes(read, 3)});
    }
    if (step != SQLITE_DONE) {
        return failure(database, "reading the tile columns");
    }
    return all;
}

} // namespace hog::store
