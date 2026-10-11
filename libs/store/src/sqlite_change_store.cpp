#include "sqlite.hpp"

#include <hog/store/sqlite_change_store.hpp>

#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace hog::store {

namespace {

using sqlite::execute;
using sqlite::failure;
using sqlite::Statement;
using sqlite::to_signed;
using sqlite::to_unsigned;

[[nodiscard]] Result<planet::Change> read_change(sqlite3_stmt* row) {
    const auto layer = sqlite3_column_int64(row, 3);
    const auto kind = sqlite3_column_int64(row, 4);
    if (layer < 0 || layer >= planet::layer_count) {
        return make_error(ErrorCode::Parse, std::format("stored layer {} is unknown", layer));
    }
    if (kind < 0 || std::cmp_greater(kind, std::numeric_limits<std::uint8_t>::max())) {
        return make_error(ErrorCode::Parse, std::format("stored change kind {} is unknown", kind));
    }
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(row, 5));
    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(row, 5));
    auto payload = planet::decode_payload(static_cast<std::uint8_t>(kind), {data, size});
    if (!payload) {
        return std::unexpected(std::move(payload.error()));
    }
    return planet::Change{.cell =
                              planet::CellId::from_raw(to_unsigned(sqlite3_column_int64(row, 0))),
                          .time = GameTime{GameDuration{sqlite3_column_int64(row, 1)}},
                          .author = planet::AuthorId{to_unsigned(sqlite3_column_int64(row, 2))},
                          .layer = static_cast<planet::Layer>(layer),
                          .payload = *std::move(payload)};
}

} // namespace

SqliteChangeStore::SqliteChangeStore(Database database) : database_{std::move(database)} {}

Result<SqliteChangeStore> SqliteChangeStore::open(const std::filesystem::path& path) {
    auto database = open_database(path);
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    return SqliteChangeStore{*std::move(database)};
}

Result<void> SqliteChangeStore::append(std::span<const planet::Change> changes) {
    sqlite3* database = database_.get();
    if (auto begun = execute(database, "BEGIN IMMEDIATE", "starting an append"); !begun) {
        return begun;
    }
    Statement insert{database, "INSERT INTO changes (cell, time, author, layer, kind, payload) "
                               "VALUES (?, ?, ?, ?, ?, ?)"};
    const auto written = [&] -> Result<void> {
        if (!insert.ok()) {
            return failure(database, "preparing an append");
        }
        for (const planet::Change& change : changes) {
            const auto payload = planet::encode_payload(change.payload);
            sqlite3_stmt* row = insert.get();
            sqlite3_bind_int64(row, 1, to_signed(change.cell.raw()));
            sqlite3_bind_int64(row, 2, change.time.time_since_epoch().count());
            sqlite3_bind_int64(row, 3, to_signed(std::to_underlying(change.author)));
            sqlite3_bind_int(row, 4, std::to_underlying(change.layer));
            sqlite3_bind_int(row, 5, std::to_underlying(change.kind()));
            // An empty vector's data may be null, which SQLite would store as NULL.
            const void* bytes = payload.empty() ? static_cast<const void*>("") : payload.data();
            sqlite3_bind_blob64(row, 6, bytes, payload.size(), SQLITE_TRANSIENT);
            if (insert.step() != SQLITE_DONE) {
                return failure(database, "appending a change");
            }
            insert.reset();
        }
        return execute(database, "COMMIT", "committing an append");
    }();
    if (!written) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return written;
}

Result<std::vector<planet::Change>> SqliteChangeStore::load() const {
    sqlite3* database = database_.get();
    Statement select{database, "SELECT cell, time, author, layer, kind, payload FROM changes "
                               "ORDER BY sequence"};
    if (!select.ok()) {
        return failure(database, "preparing a load");
    }
    std::vector<planet::Change> changes;
    int stepped = SQLITE_ROW;
    while ((stepped = select.step()) == SQLITE_ROW) {
        auto change = read_change(select.get());
        if (!change) {
            return std::unexpected(std::move(change.error()));
        }
        changes.push_back(*std::move(change));
    }
    if (stepped != SQLITE_DONE) {
        return failure(database, "loading changes");
    }
    return changes;
}

Result<std::size_t> SqliteChangeStore::erase(const planet::ChangeFilter& filter) {
    sqlite3* database = database_.get();
    Statement erase{database, "DELETE FROM changes WHERE (?1 IS NULL OR author = ?1) AND "
                              "(?2 IS NULL OR layer = ?2) AND time >= ?3 AND time < ?4"};
    if (!erase.ok()) {
        return failure(database, "preparing an erase");
    }
    sqlite3_stmt* statement = erase.get();
    if (filter.author) {
        sqlite3_bind_int64(statement, 1, to_signed(std::to_underlying(*filter.author)));
    }
    if (filter.layer) {
        sqlite3_bind_int(statement, 2, std::to_underlying(*filter.layer));
    }
    sqlite3_bind_int64(statement, 3, filter.from.time_since_epoch().count());
    sqlite3_bind_int64(statement, 4, filter.until.time_since_epoch().count());
    if (erase.step() != SQLITE_DONE) {
        return failure(database, "erasing changes");
    }
    return static_cast<std::size_t>(sqlite3_changes64(database));
}

} // namespace hog::store
