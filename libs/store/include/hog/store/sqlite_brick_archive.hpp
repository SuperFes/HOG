#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/brick_archive.hpp>
#include <hog/store/database.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace hog::store {

// A world's changed voxels kept in its SQLite file, a row a brick.
class SqliteBrickArchive final : public globe::BrickArchive {
public:
    // Opens the database at `path`, creating it if missing. The path ":memory:" opens a private
    // database that lasts as long as the archive.
    [[nodiscard]] static Result<SqliteBrickArchive> open(const std::filesystem::path& path);

    Result<void> write(std::span<const std::byte> header, std::span<const globe::SavedBrick> bricks,
                       std::span<const std::uint64_t> erased) override;
    [[nodiscard]] Result<Contents> load() const override;

private:
    explicit SqliteBrickArchive(Database database);

    Database database_;
};

} // namespace hog::store
