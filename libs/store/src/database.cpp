#include "sqlite.hpp"

#include <hog/store/database.hpp>

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace hog::store {

namespace {

using sqlite::execute;
using sqlite::failure;
using sqlite::Statement;

// Step N takes the schema from version N to N + 1. Steps are only ever appended, so every
// database ever written can be brought up to date.
constexpr std::array<std::string_view, 2> migrations{
    R"sql(
CREATE TABLE changes (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    cell INTEGER NOT NULL,
    time INTEGER NOT NULL,
    author INTEGER NOT NULL,
    layer INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    payload BLOB NOT NULL
);
CREATE INDEX changes_by_cell ON changes (cell);
CREATE INDEX changes_by_time ON changes (time);
CREATE INDEX changes_by_author ON changes (author, time);
)sql",
    R"sql(
CREATE TABLE snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    time INTEGER NOT NULL,
    data BLOB NOT NULL
);
)sql",
};

[[nodiscard]] Result<int> user_version(sqlite3* database) {
    Statement statement{database, "PRAGMA user_version"};
    if (!statement.ok() || statement.step() != SQLITE_ROW) {
        return failure(database, "reading the schema version");
    }
    return sqlite3_column_int(statement.get(), 0);
}

[[nodiscard]] Result<void> migrate_locked(sqlite3* database) {
    const auto version = user_version(database);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version < 0 || *version > schema_version()) {
        return make_error(ErrorCode::Unsupported, std::format("store schema {} is not one of 0-{}",
                                                              *version, schema_version()));
    }
    for (auto step = static_cast<std::size_t>(*version); step < migrations.size(); ++step) {
        const std::string sql{migrations.at(step)};
        if (auto migrated = execute(database, sql.c_str(),
                                    std::format("migrating the store to schema {}", step + 1));
            !migrated) {
            return migrated;
        }
    }
    const std::string set_version = std::format("PRAGMA user_version = {}", schema_version());
    return execute(database, set_version.c_str(), "setting the schema version");
}

// Holds the write lock while migrating, so connections opening the same file at once agree.
[[nodiscard]] Result<void> migrate(sqlite3* database) {
    if (auto begun = execute(database, "BEGIN IMMEDIATE", "starting a migration"); !begun) {
        return begun;
    }
    auto migrated = migrate_locked(database);
    if (migrated) {
        migrated = execute(database, "COMMIT", "committing a migration");
    }
    if (!migrated) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return migrated;
}

} // namespace

void CloseDatabase::operator()(sqlite3* database) const noexcept {
    sqlite3_close_v2(database);
}

int schema_version() noexcept {
    return static_cast<int>(migrations.size());
}

Result<Database> open_database(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    const std::u8string name = path.u8string();
    const int opened = sqlite3_open_v2(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): SQLite takes UTF-8 as char.
        reinterpret_cast<const char*>(name.c_str()), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE, nullptr);
    // SQLite allocates a handle even when opening fails, to carry the message.
    Database database{raw};
    if (opened != SQLITE_OK) {
        return failure(database.get(), std::format("opening {}", path.string()));
    }
    // Another connection may hold the write lock briefly while saving.
    sqlite3_busy_timeout(database.get(), 5'000);
    // Write-ahead logging keeps readers going during writes; in memory it is ignored.
    if (auto journal = execute(database.get(), "PRAGMA journal_mode = WAL", "enabling the WAL");
        !journal) {
        return std::unexpected(std::move(journal.error()));
    }
    if (auto migrated = migrate(database.get()); !migrated) {
        return std::unexpected(std::move(migrated.error()));
    }
    return database;
}

} // namespace hog::store
