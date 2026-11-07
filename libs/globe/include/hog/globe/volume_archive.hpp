#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/volume.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace hog::globe {

// The coarsest level a tile column is saved at besides its own: each tile one voxel.
inline constexpr int coarsest_level = std::countr_zero(static_cast<unsigned>(Volume::tile_side));

// One tile column of a volume as saved: where it stands, counted in tiles, its level, and its
// bytes. At level n its tiles are halved n times; at 0 it's the column itself.
struct SavedColumn {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint8_t level = 0;
    std::vector<std::byte> bytes;

    friend bool operator==(const SavedColumn&, const SavedColumn&) = default;
};

// Durable storage for a volume, a tile column at a time so the world needn't all be held at once,
// and a header saying what it was made from. Backends are chosen at runtime, so this is an
// interface.
class VolumeArchive {
public:
    VolumeArchive() = default;
    virtual ~VolumeArchive() = default;

    // Sets the header and writes `columns` over any in the same places, all or nothing.
    virtual Result<void> write(std::span<const std::byte> header,
                               std::span<const SavedColumn> columns) = 0;
    // Empty if nothing was ever written.
    [[nodiscard]] virtual Result<std::vector<std::byte>> header() const = 0;
    // The tile column at (`face`, `i`, `j`) at `level`, none where it was never written.
    [[nodiscard]] virtual Result<std::optional<SavedColumn>>
    column(std::uint8_t face, std::uint32_t i, std::uint32_t j, std::uint8_t level) const = 0;
    // Every tile column written, at level 0, in order.
    [[nodiscard]] virtual Result<std::vector<SavedColumn>> columns() const = 0;

protected:
    VolumeArchive(const VolumeArchive&) = default;
    VolumeArchive(VolumeArchive&&) = default;
    VolumeArchive& operator=(const VolumeArchive&) = default;
    VolumeArchive& operator=(VolumeArchive&&) = default;
};

// A volume kept in memory, for tests and worlds never saved to disk.
class MemoryVolumeArchive final : public VolumeArchive {
public:
    Result<void> write(std::span<const std::byte> header,
                       std::span<const SavedColumn> columns) override;
    [[nodiscard]] Result<std::vector<std::byte>> header() const override;
    [[nodiscard]] Result<std::optional<SavedColumn>>
    column(std::uint8_t face, std::uint32_t i, std::uint32_t j, std::uint8_t level) const override;
    [[nodiscard]] Result<std::vector<SavedColumn>> columns() const override;

private:
    std::vector<std::byte> header_;
    std::map<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint8_t>,
             std::vector<std::byte>>
        columns_;
};

inline constexpr std::uint16_t volume_format = 3;

// What a volume is made from: the planet's seed and its layers. Other planet settings aren't told
// apart yet.
struct VolumeOrigin {
    std::uint64_t seed = 0;
    Layers layers;

    friend bool operator==(const VolumeOrigin&, const VolumeOrigin&) = default;
};

[[nodiscard]] VolumeOrigin origin_of(const Generator& generator);
[[nodiscard]] std::vector<std::byte> volume_header(const VolumeOrigin& origin);
// Fails, saying why, on a header of another format, or cut short.
[[nodiscard]] Result<VolumeOrigin> origin_from(std::span<const std::byte> header);

// Whether the archive holds a volume, false where nothing was ever written; fails where it's one
// made from another origin.
[[nodiscard]] Result<bool> made_from(const VolumeArchive& archive, const VolumeOrigin& origin);

// Every tile column the archive holds, put back into a volume; fails on bytes that aren't one, or
// a volume made from another origin.
[[nodiscard]] Result<Volume> load_volume(const VolumeArchive& archive, const VolumeOrigin& origin);
// Writes the volume's tile columns not saved, all or nothing, and counts them saved; how many.
Result<std::size_t> save_volume(Volume& volume, VolumeArchive& archive, const VolumeOrigin& origin);

// The tile column at every level, its own first, as saved at (`face`, `i`, `j`).
[[nodiscard]] std::vector<SavedColumn> saved_levels(const TileColumn& column, std::uint8_t face,
                                                    std::uint32_t i, std::uint32_t j);

// Puts the tile columns with these corners into the volume where the archive holds them and the
// volume doesn't, which it says; fails on bytes that aren't one. The archive is taken to be made
// from the volume's origin.
[[nodiscard]] Result<std::vector<Site>> load_columns(Volume& volume, const VolumeArchive& archive,
                                                     std::span<const Site> corners);
// As save_volume, for those of the tile columns with these corners not saved. Every level of each
// is written.
Result<std::size_t> save_columns(Volume& volume, VolumeArchive& archive, const VolumeOrigin& origin,
                                 std::span<const Site> corners);

} // namespace hog::globe
