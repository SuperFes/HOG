#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace hog::globe {

// One tile column of a volume as saved: where it stands, counted in tiles, and its bytes.
struct SavedColumn {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
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
    // The tile column at (`face`, `i`, `j`), none where it was never written.
    [[nodiscard]] virtual Result<std::optional<SavedColumn>>
    column(std::uint8_t face, std::uint32_t i, std::uint32_t j) const = 0;

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
    [[nodiscard]] Result<std::optional<SavedColumn>> column(std::uint8_t face, std::uint32_t i,
                                                            std::uint32_t j) const override;

private:
    std::vector<std::byte> header_;
    std::map<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>, std::vector<std::byte>>
        columns_;
};

inline constexpr std::uint16_t volume_format = 1;

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

} // namespace hog::globe
