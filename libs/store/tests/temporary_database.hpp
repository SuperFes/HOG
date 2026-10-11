#pragma once

#include <hog/core/hash.hpp>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <system_error>

namespace hog::store::testing {

// A database file removed again when the test ends.
class TemporaryFile {
public:
    TemporaryFile()
        : path_{std::filesystem::temp_directory_path() /
                std::format("hog-store-{:016x}.db",
                            hog::hash_values(static_cast<std::uint64_t>(
                                std::chrono::steady_clock::now().time_since_epoch().count())))} {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        std::error_code ignored;
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path_.string() + suffix, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Runs SQL straight against the database file, behind the stores' backs.
inline void tamper(const std::filesystem::path& path, const char* sql) {
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    REQUIRE(result == SQLITE_OK);
}

// The first column of the first row the SQL returns.
inline std::int64_t query(const std::filesystem::path& path, const char* sql) {
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    std::int64_t value = -1;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

} // namespace hog::store::testing
