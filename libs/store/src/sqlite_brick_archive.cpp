#include "sqlite.hpp"

#include <hog/store/sqlite_brick_archive.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace hog::store {

using sqlite::execute;
using sqlite::failure;
using sqlite::Statement;
using sqlite::to_signed;
using sqlite::to_unsigned;

namespace {

void bind_bytes(Statement& statement, int at, std::span<const std::byte> bytes) {
    // An empty span's data may be null, which SQLite would store as NULL.
    const void* data = bytes.empty() ? static_cast<const void*>("") : bytes.data();
    sqlite3_bind_blob64(statement.get(), at, data, bytes.size(), SQLITE_STATIC);
}

std::vector<std::byte> column_bytes(const Statement& statement, int at) {
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement.get(), at));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), at));
    const std::span<const std::byte> blob{data, size};
    return {blob.begin(), blob.end()};
}

Result<void> write_locked(sqlite3* database, std::span<const std::byte> header,
                          std::span<const globe::SavedBrick> bricks,
                          std::span<const std::uint64_t> erased) {
    Statement set_header{database, "INSERT OR REPLACE INTO brick_header (id, data) VALUES (0, ?)"};
    Statement put{database, "INSERT OR REPLACE INTO bricks (key, data) VALUES (?, ?)"};
    Statement remove{database, "DELETE FROM bricks WHERE key = ?"};
    if (!set_header.ok() || !put.ok() || !remove.ok()) {
        return failure(database, "preparing a brick write");
    }
    bind_bytes(set_header, 1, header);
    if (set_header.step() != SQLITE_DONE) {
        return failure(database, "writing the brick header");
    }
    for (const globe::SavedBrick& brick : bricks) {
        put.reset();
        sqlite3_bind_int64(put.get(), 1, to_signed(brick.key));
        bind_bytes(put, 2, brick.bytes);
        if (put.step() != SQLITE_DONE) {
            return failure(database, "writing a brick");
        }
    }
    for (const std::uint64_t key : erased) {
        remove.reset();
        sqlite3_bind_int64(remove.get(), 1, to_signed(key));
        if (remove.step() != SQLITE_DONE) {
            return failure(database, "removing a brick");
        }
    }
    return {};
}

} // namespace

SqliteBrickArchive::SqliteBrickArchive(Database database) : database_{std::move(database)} {}

Result<SqliteBrickArchive> SqliteBrickArchive::open(const std::filesystem::path& path) {
    auto database = open_database(path);
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    return SqliteBrickArchive{*std::move(database)};
}

Result<void> SqliteBrickArchive::write(std::span<const std::byte> header,
                                       std::span<const globe::SavedBrick> bricks,
                                       std::span<const std::uint64_t> erased) {
    sqlite3* database = database_.get();
    if (auto begun = execute(database, "BEGIN IMMEDIATE", "starting a brick write"); !begun) {
        return begun;
    }
    auto written = write_locked(database, header, bricks, erased);
    if (written) {
        written = execute(database, "COMMIT", "committing a brick write");
    }
    if (!written) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return written;
}

Result<SqliteBrickArchive::Contents> SqliteBrickArchive::load() const {
    sqlite3* database = database_.get();
    Statement header{database, "SELECT data FROM brick_header WHERE id = 0"};
    Statement bricks{database, "SELECT key, data FROM bricks ORDER BY key"};
    if (!header.ok() || !bricks.ok()) {
        return failure(database, "preparing a brick load");
    }
    Contents contents;
    const int found = header.step();
    if (found == SQLITE_ROW) {
        contents.header = column_bytes(header, 0);
    } else if (found != SQLITE_DONE) {
        return failure(database, "reading the brick header");
    }
    int stepped = SQLITE_ROW;
    while ((stepped = bricks.step()) == SQLITE_ROW) {
        contents.bricks.push_back({.key = to_unsigned(sqlite3_column_int64(bricks.get(), 0)),
                                   .bytes = column_bytes(bricks, 1)});
    }
    if (stepped != SQLITE_DONE) {
        return failure(database, "reading bricks");
    }
    return contents;
}

} // namespace hog::store
