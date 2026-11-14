#include <hog/baked/page.hpp>
#include <hog/core/hash.hpp>

#include <algorithm>
#include <bit>
#include <string>

namespace hog::baked {
namespace {

constexpr std::uint32_t leaf_bit = 0x8000'0000U;
constexpr unsigned open_shift = 23;
constexpr std::uint32_t cube_mask = (std::uint32_t{1} << open_shift) - 1U;

std::uint32_t read32(std::span<const std::byte> bytes, std::size_t at) noexcept {
    std::array<std::byte, 4> raw{};
    std::ranges::copy(bytes.subspan(at, raw.size()), raw.begin());
    const auto value = std::bit_cast<std::uint32_t>(raw);
    return std::endian::native == std::endian::big ? std::byteswap(value) : value;
}

void write32(std::vector<std::byte>& out, std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    const auto raw = std::bit_cast<std::array<std::byte, 4>>(value);
    out.insert(out.end(), raw.begin(), raw.end());
}

void write16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(std::byte{static_cast<std::uint8_t>(value & 0xFFU)});
    out.push_back(std::byte{static_cast<std::uint8_t>(value >> 8U)});
}

std::uint16_t read16(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at]) |
                                      (std::to_integer<unsigned>(bytes[at + 1]) << 8U));
}

std::size_t octant_of(std::uint32_t x, std::uint32_t y, std::uint32_t z, unsigned bit) noexcept {
    return ((x >> bit) & 1U) | (((y >> bit) & 1U) << 1U) | (((z >> bit) & 1U) << 2U);
}

} // namespace

Result<Page> Page::view(std::span<const std::byte> bytes) {
    const auto fail = [](std::string_view why) {
        return make_error(ErrorCode::InvalidArgument, std::string{why});
    };
    if (bytes.size() < header_bytes) {
        return fail("a page's bytes are cut short");
    }
    Page page{bytes};
    if (std::to_integer<std::uint8_t>(bytes[0]) != version) {
        return fail("a page of another version");
    }
    page.size_ = std::to_integer<std::uint8_t>(bytes[1]);
    page.voxel_ = std::to_integer<std::uint8_t>(bytes[2]);
    page.flags_ = std::to_integer<std::uint8_t>(bytes[3]);
    page.root_ = read32(bytes, 4);
    page.cubes_ = read32(bytes, 8);
    page.nodes_ = read32(bytes, 12);
    if (page.voxel_ > page.size_ || page.size_ > 31) {
        return fail("a page's voxels must be no larger than it");
    }
    if (page.cubes_ > cube_mask + 1U ||
        bytes.size() != header_bytes + (std::uint64_t{page.cubes_} * cube_bytes) +
                            (std::uint64_t{page.nodes_} * node_bytes)) {
        return fail("a page's counts don't match its bytes");
    }
    // Children come before their parents, so every walk down ends.
    const auto valid = [&](std::uint32_t entry, std::uint32_t below) {
        return (entry & leaf_bit) != 0 ? (entry & cube_mask) < page.cubes_ : entry < below;
    };
    if (!valid(page.root_, page.nodes_)) {
        return fail("a page's root is out of range");
    }
    for (std::uint32_t node = 0; node < page.nodes_; ++node) {
        for (std::size_t k = 0; k < 8; ++k) {
            if (!valid(page.child(node, k), node)) {
                return fail("a page's node is out of range");
            }
        }
    }
    return page;
}

std::uint32_t Page::child(std::uint32_t node, std::size_t octant) const noexcept {
    return read32(bytes_, header_bytes + (std::size_t{cubes_} * cube_bytes) +
                              (std::size_t{node} * node_bytes) + (octant * 4));
}

Leaf Page::leaf(std::uint32_t entry, std::uint8_t size) const noexcept {
    const std::size_t at = header_bytes + (std::size_t{entry & cube_mask} * cube_bytes);
    const auto byte = [&](std::size_t k) { return std::to_integer<std::uint8_t>(bytes_[at + k]); };
    return {.cube = {.solid = static_cast<globe::Material>(byte(0)),
                     .solid_amount = read16(bytes_, at + 4),
                     .grain = static_cast<globe::Material>(byte(1)),
                     .grain_amount = read16(bytes_, at + 6),
                     .liquid = static_cast<globe::Material>(byte(2)),
                     .liquid_amount = read16(bytes_, at + 8),
                     .condition = byte(3),
                     .heat = byte(10),
                     .rise = byte(11),
                     .peak = byte(12)},
            .open = static_cast<std::uint8_t>((entry & ~leaf_bit) >> open_shift),
            .size = size};
}

Leaf Page::at(std::uint32_t x, std::uint32_t y, std::uint32_t z) const noexcept {
    std::uint32_t entry = root_;
    unsigned level = size_;
    while ((entry & leaf_bit) == 0) {
        --level;
        entry = child(entry, octant_of(x, y, z, level));
    }
    return leaf(entry, static_cast<std::uint8_t>(level));
}

std::optional<Leaf> Page::whole(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                                std::uint8_t size) const noexcept {
    std::uint32_t entry = root_;
    unsigned level = size_;
    while ((entry & leaf_bit) == 0) {
        if (level <= size) {
            return std::nullopt;
        }
        --level;
        entry = child(entry, octant_of(x, y, z, level));
    }
    return leaf(entry, static_cast<std::uint8_t>(level));
}

std::size_t PageBuilder::CubeHash::operator()(const globe::Cube& cube) const noexcept {
    const std::array<std::uint16_t, 10> parts{static_cast<std::uint16_t>(cube.solid),
                                              cube.solid_amount,
                                              static_cast<std::uint16_t>(cube.grain),
                                              cube.grain_amount,
                                              static_cast<std::uint16_t>(cube.liquid),
                                              cube.liquid_amount,
                                              cube.condition,
                                              cube.heat,
                                              cube.rise,
                                              cube.peak};
    return static_cast<std::size_t>(hash_bytes(std::as_bytes(std::span{parts})));
}

std::size_t
PageBuilder::NodeHash::operator()(const std::array<std::uint32_t, 8>& node) const noexcept {
    return static_cast<std::size_t>(hash_bytes(std::as_bytes(std::span{node})));
}

std::uint32_t PageBuilder::leaf(const globe::Cube& cube, std::uint8_t open) {
    const auto [found, added] =
        cube_ids_.try_emplace(cube, static_cast<std::uint32_t>(cubes_.size()));
    if (added) {
        cubes_.push_back(cube);
    }
    return leaf_bit | (std::uint32_t{open} << open_shift) | found->second;
}

std::uint32_t PageBuilder::inner(const std::array<std::uint32_t, 8>& children) {
    if ((children[0] & leaf_bit) != 0 &&
        std::ranges::all_of(children, [&](std::uint32_t c) { return c == children[0]; })) {
        return children[0];
    }
    const auto [found, added] =
        node_ids_.try_emplace(children, static_cast<std::uint32_t>(nodes_.size()));
    if (added) {
        nodes_.push_back(children);
    }
    return found->second;
}

std::vector<std::byte> PageBuilder::finish(std::uint32_t root, std::uint8_t size,
                                           std::uint8_t voxel, std::uint8_t flags) const {
    std::vector<std::byte> out;
    out.reserve(Page::header_bytes + (cubes_.size() * Page::cube_bytes) +
                (nodes_.size() * Page::node_bytes));
    out.push_back(std::byte{Page::version});
    out.push_back(std::byte{size});
    out.push_back(std::byte{voxel});
    out.push_back(std::byte{flags});
    write32(out, root);
    write32(out, static_cast<std::uint32_t>(cubes_.size()));
    write32(out, static_cast<std::uint32_t>(nodes_.size()));
    for (const globe::Cube& cube : cubes_) {
        out.push_back(std::byte{static_cast<std::uint8_t>(cube.solid)});
        out.push_back(std::byte{static_cast<std::uint8_t>(cube.grain)});
        out.push_back(std::byte{static_cast<std::uint8_t>(cube.liquid)});
        out.push_back(std::byte{cube.condition});
        write16(out, cube.solid_amount);
        write16(out, cube.grain_amount);
        write16(out, cube.liquid_amount);
        out.push_back(std::byte{cube.heat});
        out.push_back(std::byte{cube.rise});
        out.push_back(std::byte{cube.peak});
    }
    for (const auto& node : nodes_) {
        for (const std::uint32_t entry : node) {
            write32(out, entry);
        }
    }
    return out;
}

std::vector<std::byte> encode_one(std::uint8_t size, std::uint8_t voxel, std::uint8_t flags,
                                  const globe::Cube& cube, std::uint8_t open) {
    PageBuilder builder;
    const std::uint32_t root = builder.leaf(cube, open);
    return builder.finish(root, size, voxel, flags);
}

std::vector<std::byte> encode_tree(const globe::VoxelTree& tree, std::uint8_t size,
                                   std::uint8_t voxel, std::uint8_t flags) {
    return encode(size, voxel, flags,
                  [&](std::uint32_t x, std::uint32_t y, std::uint32_t z,
                      std::uint8_t cells) -> std::optional<Leaf> {
                      const auto [px, py, pz] =
                          std::array{static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)};
                      const int across = 1 << cells;
                      if (cells != 0 && tree.leaf_side(px, py, pz) < across) {
                          return std::nullopt;
                      }
                      return Leaf{.cube = tree.at(px, py, pz),
                                  .open = tree.coarsest(px, py, pz, across)};
                  });
}

std::vector<std::byte> encode_part(const Page& page, std::uint32_t x, std::uint32_t y,
                                   std::uint32_t z, std::uint8_t size) {
    // A part within one voxel is that voxel whole.
    const auto voxel = std::min(page.voxel(), size);
    return encode(size, voxel, page.flags(),
                  [&](std::uint32_t cx, std::uint32_t cy, std::uint32_t cz,
                      std::uint8_t cells) -> std::optional<Leaf> {
                      return page.whole(x + (cx << voxel), y + (cy << voxel), z + (cz << voxel),
                                        static_cast<std::uint8_t>(cells + voxel));
                  });
}

} // namespace hog::baked
