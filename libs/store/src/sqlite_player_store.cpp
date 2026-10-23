#include "sqlite.hpp"

#include <hog/store/sqlite_player_store.hpp>

#include <bit>
#include <utility>

namespace hog::store {

using sqlite::failure;
using sqlite::Statement;

namespace {

// SQLite's integers are signed; accounts are kept as their bits.
[[nodiscard]] sqlite3_int64 key_of(std::uint64_t account) noexcept {
    return std::bit_cast<sqlite3_int64>(account);
}

} // namespace

SqlitePlayerStore::SqlitePlayerStore(Database database) : database_{std::move(database)} {}

Result<SqlitePlayerStore> SqlitePlayerStore::open(const std::filesystem::path& path) {
    auto database = open_database(path);
    if (!database) {
        return std::unexpected(std::move(database.error()));
    }
    return SqlitePlayerStore{*std::move(database)};
}

Result<void> SqlitePlayerStore::keep(std::uint64_t account, const PlayerPlace& place) {
    sqlite3* database = database_.get();
    Statement insert{database,
                     "INSERT OR REPLACE INTO players (account, x, y, z, flying, heading, pitch) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?)"};
    if (!insert.ok()) {
        return failure(database, "preparing a player's place");
    }
    sqlite3_bind_int64(insert.get(), 1, key_of(account));
    sqlite3_bind_int64(insert.get(), 2, place.at.x);
    sqlite3_bind_int64(insert.get(), 3, place.at.y);
    sqlite3_bind_int64(insert.get(), 4, place.at.z);
    sqlite3_bind_int(insert.get(), 5, place.flying ? 1 : 0);
    sqlite3_bind_int(insert.get(), 6, place.heading);
    sqlite3_bind_int(insert.get(), 7, place.pitch);
    if (insert.step() != SQLITE_DONE) {
        return failure(database, "keeping a player's place");
    }
    return {};
}

Result<std::optional<PlayerPlace>> SqlitePlayerStore::find(std::uint64_t account) const {
    sqlite3* database = database_.get();
    Statement select{database,
                     "SELECT x, y, z, flying, heading, pitch FROM players WHERE account = ?"};
    if (!select.ok()) {
        return failure(database, "preparing to find a player's place");
    }
    sqlite3_bind_int64(select.get(), 1, key_of(account));
    const int stepped = select.step();
    if (stepped == SQLITE_DONE) {
        return std::nullopt;
    }
    if (stepped != SQLITE_ROW) {
        return failure(database, "finding a player's place");
    }
    sqlite3_stmt* row = select.get();
    return PlayerPlace{
        .at = {sqlite3_column_int64(row, 0), sqlite3_column_int64(row, 1),
               sqlite3_column_int64(row, 2)},
        .flying = sqlite3_column_int(row, 3) != 0,
        .heading = static_cast<std::uint16_t>(sqlite3_column_int(row, 4)),
        .pitch = static_cast<std::int16_t>(sqlite3_column_int(row, 5)),
    };
}

} // namespace hog::store
