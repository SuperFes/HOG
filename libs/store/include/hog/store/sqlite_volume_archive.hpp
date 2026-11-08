#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/store/database.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace hog::store {

// A world's volume kept in its SQLite file, a row a tile column at each level, and a row a tile.
class SqliteVolumeArchive final : public globe::VolumeArchive {
public:
    // Opens the database at `path`, creating it if missing. The path ":memory:" opens a private
    // database that lasts as long as the archive.
    [[nodiscard]] static Result<SqliteVolumeArchive> open(const std::filesystem::path& path);

    Result<void> write(std::span<const std::byte> header,
                       std::span<const globe::SavedColumn> columns) override;
    [[nodiscard]] Result<std::vector<std::byte>> header() const override;
    [[nodiscard]] Result<std::optional<globe::SavedColumn>>
    column(std::uint8_t face, std::uint32_t i, std::uint32_t j, std::uint8_t level) const override;
    [[nodiscard]] Result<std::vector<globe::SavedColumn>> columns() const override;
    Result<void> write_tiles(std::span<const std::byte> header,
                             std::span<const globe::SavedTile> tiles) override;
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>>
    tile(globe::TileKey key) const override;
    [[nodiscard]] Result<std::vector<std::uint32_t>> tiles_up(globe::TileKey base) const override;
    [[nodiscard]] Result<std::vector<globe::TileKey>> tiles_at(std::uint8_t level) const override;

private:
    explicit SqliteVolumeArchive(Database database);

    Database database_;
};

} // namespace hog::store
