#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel_store.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace hog::globe {

// One brick as saved: its key and its bytes.
struct SavedBrick {
    std::uint64_t key = 0;
    std::vector<std::byte> bytes;

    friend bool operator==(const SavedBrick&, const SavedBrick&) = default;
};

// Durable storage for a store's bricks, each kept apart so a save writes only what changed, and
// a header saying what they stand on. Backends are chosen at runtime, so this is an interface.
class BrickArchive {
public:
    BrickArchive() = default;
    virtual ~BrickArchive() = default;

    // Sets the header, writes `bricks` over any of the same keys and removes `erased`, all or
    // nothing.
    virtual Result<void> write(std::span<const std::byte> header,
                               std::span<const SavedBrick> bricks,
                               std::span<const std::uint64_t> erased) = 0;

    struct Contents {
        std::vector<std::byte> header; // empty if nothing was ever written
        std::vector<SavedBrick> bricks;
    };
    [[nodiscard]] virtual Result<Contents> load() const = 0;

protected:
    BrickArchive(const BrickArchive&) = default;
    BrickArchive(BrickArchive&&) = default;
    BrickArchive& operator=(const BrickArchive&) = default;
    BrickArchive& operator=(BrickArchive&&) = default;
};

// Bricks kept in memory, for tests and worlds never saved to disk.
class MemoryBrickArchive final : public BrickArchive {
public:
    Result<void> write(std::span<const std::byte> header, std::span<const SavedBrick> bricks,
                       std::span<const std::uint64_t> erased) override;
    [[nodiscard]] Result<Contents> load() const override;

    // What's kept, open to tests that damage it.
    [[nodiscard]] std::map<std::uint64_t, std::vector<std::byte>>& bricks() noexcept {
        return bricks_;
    }

private:
    std::vector<std::byte> header_;
    std::map<std::uint64_t, std::vector<std::byte>> bricks_;
};

// The format bricks are saved in; it changes only when old saves still read.
inline constexpr std::uint16_t brick_format = 1;

// Writes the bricks changed in the store since it was last saved; returns how many.
[[nodiscard]] Result<std::size_t> save(VoxelStore& store, BrickArchive& archive);

// The store saved in the archive, empty if nothing was. Fails if it was saved on other layers,
// whose voxels would stand elsewhere, or if anything saved doesn't read.
[[nodiscard]] Result<VoxelStore> load(const Layers& layers, const BrickArchive& archive);

} // namespace hog::globe
