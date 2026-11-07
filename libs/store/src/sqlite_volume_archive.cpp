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
    if (!set_header.ok() || !put.ok()) {
        return failure(database, "preparing a volume write");
    }
    bind_bytes(set_header, 1, header);
    if (set_header.step() != SQLITE_DONE) {
        return failure(database, "writing the volume header");
    }
    for (const globe::SavedColumn& column : columns) {
        put.reset();
        sqlite3_bind_int(put.get(), 1, column.face);
        sqlite3_bind_int64(put.get(), 2, column.i);
        sqlite3_bind_int64(put.get(), 3, column.j);
        bind_bytes(put, 4, column.bytes);
        if (put.step() != SQLITE_DONE) {
            return failure(database, "writing a tile column");
        }
    }
    return {};
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
    if (auto begun = execute(database, "BEGIN IMMEDIATE", "starting a volume write"); !begun) {
        return begun;
    }
    auto written = write_locked(database, header, columns);
    if (written) {
        written = execute(database, "COMMIT", "committing a volume write");
    }
    if (!written) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return written;
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

Result<std::optional<globe::SavedColumn>>
SqliteVolumeArchive::column(std::uint8_t face, std::uint32_t i, std::uint32_t j) const {
    sqlite3* database = database_.get();
    Statement read{database, "SELECT data FROM volume_columns WHERE face = ? AND i = ? AND j = ?"};
    if (!read.ok()) {
        return failure(database, "preparing a tile column read");
    }
    sqlite3_bind_int(read.get(), 1, face);
    sqlite3_bind_int64(read.get(), 2, i);
    sqlite3_bind_int64(read.get(), 3, j);
    const int found = read.step();
    if (found == SQLITE_ROW) {
        return globe::SavedColumn{.face = face, .i = i, .j = j, .bytes = column_bytes(read, 0)};
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
                       .bytes = column_bytes(read, 3)});
    }
    if (step != SQLITE_DONE) {
        return failure(database, "reading the tile columns");
    }
    return all;
}

} // namespace hog::store
