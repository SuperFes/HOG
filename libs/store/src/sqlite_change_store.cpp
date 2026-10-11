#include <hog/store/sqlite_change_store.hpp>

#include <sqlite3.h>

#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace hog::store {

namespace {

constexpr int schema_version = 1;

constexpr std::string_view schema = R"sql(
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
PRAGMA user_version = 1;
)sql";

[[nodiscard]] std::unexpected<Error> failure(sqlite3* database, std::string_view doing) {
    return make_error(ErrorCode::Io, std::format("{}: {}", doing, sqlite3_errmsg(database)));
}

// SQLite integers are signed; ids and authors keep all 64 bits.
[[nodiscard]] std::int64_t to_signed(std::uint64_t value) noexcept {
    return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t to_unsigned(std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql) {
        // On failure the statement stays null, which `ok` reports.
        sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_,
                           nullptr);
    }
    Statement(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& operator=(Statement&&) = delete;
    ~Statement() { sqlite3_finalize(statement_); }

    [[nodiscard]] bool ok() const noexcept { return statement_ != nullptr; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

    // SQLITE_ROW, SQLITE_DONE or an error code.
    int step() noexcept { return sqlite3_step(statement_); }

    void reset() noexcept {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

[[nodiscard]] Result<void> execute(sqlite3* database, const char* sql, std::string_view doing) {
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return failure(database, doing);
    }
    return {};
}

[[nodiscard]] Result<int> user_version(sqlite3* database) {
    Statement statement{database, "PRAGMA user_version"};
    if (!statement.ok() || statement.step() != SQLITE_ROW) {
        return failure(database, "reading the schema version");
    }
    return sqlite3_column_int(statement.get(), 0);
}

[[nodiscard]] Result<void> prepare_schema(sqlite3* database) {
    const auto version = user_version(database);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version == schema_version) {
        return {};
    }
    if (*version != 0) {
        return make_error(ErrorCode::Unsupported, std::format("change store schema {} is not {}",
                                                              *version, schema_version));
    }
    const std::string script = std::format("BEGIN;{}COMMIT;", schema);
    if (auto created = execute(database, script.c_str(), "creating the change store"); !created) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        return created;
    }
    return {};
}

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

void SqliteChangeStore::Close::operator()(sqlite3* database) const noexcept {
    sqlite3_close_v2(database);
}

SqliteChangeStore::SqliteChangeStore(std::unique_ptr<sqlite3, Close> database)
    : database_{std::move(database)} {}

Result<SqliteChangeStore> SqliteChangeStore::open(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    const std::u8string name = path.u8string();
    const int opened = sqlite3_open_v2(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): SQLite takes UTF-8 as char.
        reinterpret_cast<const char*>(name.c_str()), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE, nullptr);
    // SQLite allocates a handle even when opening fails, to carry the message.
    std::unique_ptr<sqlite3, Close> database{raw};
    if (opened != SQLITE_OK) {
        return failure(database.get(), std::format("opening {}", path.string()));
    }
    // Write-ahead logging keeps readers going during writes; in memory it is ignored.
    if (auto journal = execute(database.get(), "PRAGMA journal_mode = WAL", "enabling the WAL");
        !journal) {
        return std::unexpected(std::move(journal.error()));
    }
    if (auto prepared = prepare_schema(database.get()); !prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    return SqliteChangeStore{std::move(database)};
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
