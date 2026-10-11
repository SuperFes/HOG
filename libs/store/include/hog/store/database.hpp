#pragma once

#include <hog/core/result.hpp>

#include <filesystem>
#include <memory>

struct sqlite3;

namespace hog::store {

struct CloseDatabase {
    void operator()(sqlite3* database) const noexcept;
};

// One connection to a world's SQLite file. Each store holds its own connection to the same file.
using Database = std::unique_ptr<sqlite3, CloseDatabase>;

// Opens the world database at `path`, creating it if missing and bringing its schema up to date.
// The path ":memory:" opens a private database that lasts as long as the connection. Fails with
// Unsupported on a schema newer than this build knows.
[[nodiscard]] Result<Database> open_database(const std::filesystem::path& path);

// The schema version this build creates.
[[nodiscard]] int schema_version() noexcept;

} // namespace hog::store
