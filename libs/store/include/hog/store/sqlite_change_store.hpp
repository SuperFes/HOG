#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/change_store.hpp>
#include <hog/store/database.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace hog::store {

// Changes kept in one SQLite database file.
class SqliteChangeStore final : public planet::ChangeStore {
public:
    // Opens the database at `path`, creating it if missing. The path ":memory:" opens a private
    // database that lasts as long as the store.
    [[nodiscard]] static Result<SqliteChangeStore> open(const std::filesystem::path& path);

    Result<void> append(std::span<const planet::Change> changes) override;
    [[nodiscard]] Result<std::vector<planet::Change>> load() const override;
    Result<std::size_t> erase(const planet::ChangeFilter& filter) override;

private:
    explicit SqliteChangeStore(Database database);

    Database database_;
};

} // namespace hog::store
