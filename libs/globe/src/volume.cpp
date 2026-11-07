#include <hog/core/bytes.hpp>
#include <hog/globe/volume.hpp>

#include <cstdint>
#include <iterator>
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

Volume::Column Volume::column_of(Site site) noexcept {
    return {.face = site.face, .i = site.i / side, .j = site.j / side};
}

std::optional<std::uint32_t> Volume::floor(Site site) const {
    const auto found = floors_.find(column_of(site));
    return found == floors_.end() ? std::nullopt : std::optional{found->second};
}

void Volume::set_floor(Site site, std::uint32_t layer) {
    floors_.insert_or_assign(column_of(site), layer);
    unsaved_.insert(column_of(site));
}

bool Volume::made(Site site) const {
    const std::optional<std::uint32_t> lowest = floor(site);
    return lowest && site.layer >= *lowest;
}

Cube Volume::at(Site site) const {
    const auto found = tiles_.find(key_of(site));
    return found == tiles_.end()
               ? Cube{}
               : found->second.at(within(site.i), within(site.j), within(site.layer));
}

bool Volume::set(Site site, const Cube& voxel) {
    if (!made(site)) {
        return false;
    }
    const Key key = key_of(site);
    auto found = tiles_.find(key);
    if (found == tiles_.end()) {
        if (voxel == Cube{}) {
            return true;
        }
        found = tiles_.emplace(key, VoxelTree{tile_side}).first;
    }
    VoxelTree& tile = found->second;
    if (tile.at(within(site.i), within(site.j), within(site.layer)) == voxel) {
        return true;
    }
    tile.set(within(site.i), within(site.j), within(site.layer), voxel);
    if (all_air(tile)) {
        tiles_.erase(found);
    }
    // Runs of sets within a brick are logged once, at the last.
    const BrickSite brick = brick_of(site);
    ++revision_;
    if (!log_.empty() && log_.back().second == brick) {
        log_.back().first = revision_;
    } else {
        log_.emplace_back(revision_, brick);
    }
    unsaved_.insert(column_of(site));
    return true;
}

bool Volume::put_on(Site below, const Cube& voxel) {
    const Site above{.face = below.face, .i = below.i, .j = below.j, .layer = below.layer + 1};
    if (!made(below) || at(below).solid == Material::Air || at(above).solid != Material::Air) {
        return false;
    }
    return set(above, voxel);
}

const VoxelTree* Volume::tile_at(Site site) const {
    const auto found = tiles_.find(key_of(site));
    return found == tiles_.end() ? nullptr : &found->second;
}

void Volume::put_tile(Site site, VoxelTree tile) {
    unsaved_.insert(column_of(site));
    if (all_air(tile)) {
        tiles_.erase(key_of(site));
        return;
    }
    tiles_.insert_or_assign(key_of(site), std::move(tile));
}

void Volume::merge(Volume other) {
    for (const auto& [column, layer] : other.floors_) {
        floors_.insert_or_assign(column, layer);
    }
    for (auto& [key, tile] : other.tiles_) {
        tiles_.insert_or_assign(key, std::move(tile));
    }
}

std::vector<std::byte> Volume::encode() const {
    ByteWriter out;
    out.write_varint(floors_.size());
    for (const auto& [column, layer] : floors_) {
        out.write(column.face);
        out.write_varint(column.i);
        out.write_varint(column.j);
        out.write_varint(layer);
    }
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
    Volume volume;
    const auto floors = in.read_varint();
    if (!floors || *floors > bytes.size()) {
        return fail("a volume's floor count is cut short or out of range");
    }
    for (std::uint64_t n = 0; n < *floors; ++n) {
        const auto face = in.read<std::uint8_t>();
        const auto i = in.read_varint();
        const auto j = in.read_varint();
        const auto layer = in.read_varint();
        if (!face || !i || !j || !layer) {
            return fail("a volume's floors are cut short");
        }
        volume.floors_.emplace(Column{.face = *face,
                                      .i = static_cast<std::uint32_t>(*i),
                                      .j = static_cast<std::uint32_t>(*j)},
                               static_cast<std::uint32_t>(*layer));
    }
    const auto count = in.read_varint();
    if (!count || *count > bytes.size()) {
        return fail("a volume's tile count is cut short or out of range");
    }
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

std::vector<std::byte> Volume::encode_column(Site site) const {
    const std::optional<std::uint32_t> lowest = floor(site);
    if (!lowest) {
        return {};
    }
    const Column column = column_of(site);
    const auto first = tiles_.lower_bound({.face = column.face, .i = column.i, .j = column.j});
    const auto last =
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX});
    ByteWriter out;
    out.write_varint(*lowest);
    out.write_varint(static_cast<std::uint64_t>(std::distance(first, last)));
    for (auto at = first; at != last; ++at) {
        const std::vector<std::byte> bytes = at->second.encode();
        out.write_varint(at->first.k);
        out.write_varint(bytes.size());
        out.write_bytes(bytes);
    }
    return std::move(out).take();
}

Result<void> Volume::decode_column(Site site, std::span<const std::byte> bytes) {
    const auto fail = [](std::string_view why) {
        return make_error(ErrorCode::InvalidArgument, std::string{why});
    };
    ByteReader in{bytes};
    const auto lowest = in.read_varint();
    const auto count = in.read_varint();
    if (!lowest || !count || *count > bytes.size()) {
        return fail("a tile column's bytes are cut short or out of range");
    }
    const Column column = column_of(site);
    std::vector<std::pair<Key, VoxelTree>> found;
    for (std::uint64_t n = 0; n < *count; ++n) {
        const auto k = in.read_varint();
        const auto size = in.read_varint();
        if (!k || !size) {
            return fail("a tile column's bytes are cut short");
        }
        const auto tree_bytes = in.read_bytes(*size);
        if (!tree_bytes) {
            return fail("a tile column's bytes are cut short");
        }
        auto tree = VoxelTree::decode(*tree_bytes);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        if (tree->side() != tile_side) {
            return fail("a tile column's tile is the wrong size");
        }
        found.emplace_back(Key{.face = column.face,
                               .i = column.i,
                               .j = column.j,
                               .k = static_cast<std::uint32_t>(*k)},
                           std::move(*tree));
    }
    if (in.remaining() != 0) {
        return fail("a tile column's bytes run on past its tiles");
    }
    tiles_.erase(
        tiles_.lower_bound({.face = column.face, .i = column.i, .j = column.j}),
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX}));
    for (auto& [key, tree] : found) {
        tiles_.insert_or_assign(key, std::move(tree));
    }
    floors_.insert_or_assign(column, static_cast<std::uint32_t>(*lowest));
    unsaved_.erase(column);
    return {};
}

bool Volume::same_as(const Volume& other) const {
    if (floors_ != other.floors_ || tiles_.size() != other.tiles_.size()) {
        return false;
    }
    for (auto a = tiles_.begin(), b = other.tiles_.begin(); a != tiles_.end(); ++a, ++b) {
        if (a->first != b->first || !a->second.same_as(b->second)) {
            return false;
        }
    }
    return true;
}

std::vector<BrickSite> Volume::changed_since(std::uint64_t revision) const {
    std::set<BrickSite> bricks;
    for (auto entry = log_.rbegin(); entry != log_.rend() && entry->first > revision; ++entry) {
        bricks.insert(entry->second);
    }
    return {bricks.begin(), bricks.end()};
}

std::vector<Site> Volume::unsaved() const {
    std::vector<Site> corners;
    corners.reserve(unsaved_.size());
    for (const Column& column : unsaved_) {
        corners.push_back({.face = column.face, .i = column.i * side, .j = column.j * side});
    }
    return corners;
}

} // namespace hog::globe
