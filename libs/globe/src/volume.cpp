#include <hog/globe/volume.hpp>

#include <hog/core/bytes.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace hog::globe {
namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

int within(std::uint32_t at) {
    return static_cast<int>(at % side);
}

bool all_air(const VoxelTree& tile) {
    return tile.leaf_side(0, 0, 0) == Volume::tile_side && tile.at(0, 0, 0) == Cube{};
}

} // namespace

Volume::Key Volume::key_of(Site site) noexcept {
    return {.face = site.face, .i = site.i / side, .j = site.j / side, .k = site.layer / side};
}

Cube Volume::at(Site site) const {
    const auto found = tiles_.find(key_of(site));
    return found == tiles_.end() ? Cube{}
                                 : found->second.at(within(site.i), within(site.j),
                                                    within(site.layer));
}

void Volume::set(Site site, const Cube& voxel) {
    const Key key = key_of(site);
    auto found = tiles_.find(key);
    if (found == tiles_.end()) {
        if (voxel == Cube{}) {
            return;
        }
        found = tiles_.emplace(key, VoxelTree{tile_side}).first;
    }
    VoxelTree& tile = found->second;
    tile.set(within(site.i), within(site.j), within(site.layer), voxel);
    if (all_air(tile)) {
        tiles_.erase(found);
    }
}

bool Volume::put_on(Site below, const Cube& voxel) {
    const Site above{.face = below.face, .i = below.i, .j = below.j, .layer = below.layer + 1};
    if (at(below).solid == Material::Air || at(above).solid != Material::Air) {
        return false;
    }
    set(above, voxel);
    return true;
}

const VoxelTree* Volume::tile_at(Site site) const {
    const auto found = tiles_.find(key_of(site));
    return found == tiles_.end() ? nullptr : &found->second;
}

void Volume::put_tile(Site site, VoxelTree tile) {
    if (all_air(tile)) {
        tiles_.erase(key_of(site));
        return;
    }
    tiles_.insert_or_assign(key_of(site), std::move(tile));
}

void Volume::merge(Volume&& other) {
    for (auto& [key, tile] : other.tiles_) {
        tiles_.insert_or_assign(key, std::move(tile));
    }
    other.tiles_.clear();
}

std::vector<std::byte> Volume::encode() const {
    ByteWriter out;
    out.write_varint(tiles_.size());
    for (const auto& [key, tile] : tiles_) {
        out.write(key.face);
        out.write_varint(key.i);
        out.write_varint(key.j);
        out.write_varint(key.k);
        const std::vector<std::byte> bytes = tile.encode();
        out.write_varint(bytes.size());
        out.write_bytes(bytes);
    }
    return std::move(out).take();
}

Result<Volume> Volume::decode(std::span<const std::byte> bytes) {
    const auto fail = [](std::string_view why) {
        return make_error(ErrorCode::InvalidArgument, std::string{why});
    };
    ByteReader in{bytes};
    const auto count = in.read_varint();
    if (!count || *count > bytes.size()) {
        return fail("a volume's tile count is cut short or out of range");
    }
    Volume volume;
    for (std::uint64_t n = 0; n < *count; ++n) {
        const auto face = in.read<std::uint8_t>();
        const auto i = in.read_varint();
        const auto j = in.read_varint();
        const auto k = in.read_varint();
        const auto size = in.read_varint();
        if (!face || !i || !j || !k || !size) {
            return fail("a volume's bytes are cut short");
        }
        const auto tree_bytes = in.read_bytes(*size);
        if (!tree_bytes) {
            return fail("a volume's bytes are cut short");
        }
        auto tree = VoxelTree::decode(*tree_bytes);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        if (tree->side() != tile_side) {
            return fail("a volume's tile is the wrong size");
        }
        volume.tiles_.emplace(Key{.face = *face,
                                  .i = static_cast<std::uint32_t>(*i),
                                  .j = static_cast<std::uint32_t>(*j),
                                  .k = static_cast<std::uint32_t>(*k)},
                              std::move(*tree));
    }
    if (in.remaining() != 0) {
        return fail("a volume's bytes run on past its tiles");
    }
    return volume;
}

bool Volume::same_as(const Volume& other) const {
    if (tiles_.size() != other.tiles_.size()) {
        return false;
    }
    for (auto a = tiles_.begin(), b = other.tiles_.begin(); a != tiles_.end(); ++a, ++b) {
        if (a->first != b->first || !a->second.same_as(b->second)) {
            return false;
        }
    }
    return true;
}

} // namespace hog::globe
