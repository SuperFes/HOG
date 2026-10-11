#pragma once

#include <hog/core/result.hpp>

#include <sqlite3.h>

#include <bit>
#include <cstdint>
#include <format>
#include <string_view>

namespace hog::store::sqlite {

[[nodiscard]] inline std::unexpected<Error> failure(sqlite3* database, std::string_view doing) {
    return make_error(ErrorCode::Io, std::format("{}: {}", doing, sqlite3_errmsg(database)));
}

// SQLite integers are signed; ids and authors keep all 64 bits.
[[nodiscard]] inline std::int64_t to_signed(std::uint64_t value) noexcept {
    return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] inline std::uint64_t to_unsigned(std::int64_t value) noexcept {
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

[[nodiscard]] inline Result<void> execute(sqlite3* database, const char* sql,
                                          std::string_view doing) {
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return failure(database, doing);
    }
    return {};
}

} // namespace hog::store::sqlite
