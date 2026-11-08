#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/globe/volume.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hog::globe {
namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

int within(std::uint32_t at) {
    return static_cast<int>(at % side);
}

} // namespace

std::vector<BrickSite> brick_footprint(Site corner) {
    constexpr std::uint32_t across = side / brick_side;
    std::vector<BrickSite> bricks;
    bricks.reserve(std::size_t{across} * across);
    for (std::uint32_t j = 0; j < across; ++j) {
        for (std::uint32_t i = 0; i < across; ++i) {
            bricks.push_back({.face = corner.face,
                              .i = (corner.i / brick_side) + i,
                              .j = (corner.j / brick_side) + j});
        }
    }
    return bricks;
}

Volume::Key Volume::key_of(Site site) noexcept {
    return {.face = site.face, .i = site.i / side, .j = site.j / side, .k = site.layer / side};
}

Volume::Column Volume::column_of(Site site) noexcept {
    return {.face = site.face, .i = site.i / side, .j = site.j / side};
}

std::optional<std::uint32_t> Volume::floor(Site site) const {
    const Column column = column_of(site);
    const auto lowest = tiles_.lower_bound({.face = column.face, .i = column.i, .j = column.j});
    if (lowest == tiles_.end() || lowest->first.level != 0 || lowest->first.face != column.face ||
        lowest->first.i != column.i || lowest->first.j != column.j) {
        return std::nullopt;
    }
    return lowest->first.k * side;
}

void Volume::set_floor(Site site, std::uint32_t layer) {
    const Key key = key_of({.face = site.face, .i = site.i, .j = site.j, .layer = layer});
    const std::optional<std::uint32_t> lowest = floor(site);
    for (std::uint32_t k = key.k; k < (lowest ? *lowest / side : key.k + 1); ++k) {
        tiles_.try_emplace({.face = key.face, .i = key.i, .j = key.j, .k = k}, tile_side);
    }
    unsaved_.insert(column_of(site));
}

bool Volume::made(Site site) const {
    const std::optional<std::uint32_t> lowest = floor(site);
    return lowest && site.layer >= *lowest;
}

std::uint32_t Volume::held_top(Site site) const {
    const Column column = column_of(site);
    const auto past =
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX});
    if (past != tiles_.begin()) {
        const Key& last = std::prev(past)->first;
        if (last.face == column.face && last.i == column.i && last.j == column.j) {
            return (last.k + 1) * side;
        }
    }
    return floor(site).value_or(0);
}

std::optional<std::uint32_t> Volume::top(Site site) const {
    const Column column = column_of(site);
    auto past =
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX});
    for (; past != tiles_.begin(); --past) {
        const auto& [key, tree] = *std::prev(past);
        if (key.face != column.face || key.i != column.i || key.j != column.j) {
            break;
        }
        for (int layer = Volume::tile_side - 1; layer >= 0; --layer) {
            const Cube cube = tree.at(within(site.i), within(site.j), layer);
            if (cube.solid != Material::Air || cube.grain != Material::Air ||
                cube.liquid != Material::Air) {
                return (key.k * side) + static_cast<std::uint32_t>(layer);
            }
        }
    }
    return std::nullopt;
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
    tiles_.insert_or_assign(key_of(site), std::move(tile));
}

const VoxelTree* Volume::tile(TileKey key) const {
    const auto found = tiles_.find(key);
    return found == tiles_.end() ? nullptr : &found->second;
}

void Volume::put_coarse(TileKey key, VoxelTree tile) {
    HOG_ASSERT(key.level > 0, "a tile coarser than the finest");
    coarsest_ = std::max(coarsest_, key.level);
    tiles_.insert_or_assign(key, std::move(tile));
}

std::optional<std::pair<std::uint8_t, const VoxelTree*>>
Volume::finest_over(Site site, std::uint8_t level) const {
    // Tiles are as many voxels across at every level, so a level coarser is a tile twice as wide.
    TileKey key = key_of(site);
    for (std::uint8_t at = 0; at < level; ++at) {
        key = key.parent();
    }
    for (std::uint8_t at = level; at <= coarsest_; ++at, key = key.parent()) {
        if (const VoxelTree* found = tile(key)) {
            return std::pair{at, found};
        }
    }
    return std::nullopt;
}

Cube Volume::finest_at(Site site) const {
    const auto over = finest_over(site);
    if (!over) {
        return {};
    }
    const auto [level, found] = *over;
    const auto place = [&](std::uint32_t at) { return within(at >> level); };
    return found->at(place(site.i), place(site.j), place(site.layer));
}

void Volume::refresh_over(TileKey key, std::uint8_t top) {
    for (TileKey parent = key.parent(); parent.level <= top; parent = parent.parent()) {
        std::array<Octant, 8> children{};
        for (std::size_t octant = 0; octant < children.size(); ++octant) {
            children.at(octant).tree = tile(parent.child(octant));
        }
        put_coarse(parent, parent_of(children, tile_side, tile(parent)));
    }
}

void Volume::merge(Volume other) {
    for (auto& [key, tile] : other.tiles_) {
        tiles_.insert_or_assign(key, std::move(tile));
    }
    coarsest_ = std::max(coarsest_, other.coarsest_);
}

std::vector<std::byte> Volume::encode() const {
    ByteWriter out;
    out.write_varint(tiles_.size());
    for (const auto& [key, tile] : tiles_) {
        out.write(key.level);
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
    const auto count = in.read_varint();
    if (!count || *count > bytes.size()) {
        return fail("a volume's tile count is cut short or out of range");
    }
    for (std::uint64_t n = 0; n < *count; ++n) {
        const auto level = in.read<std::uint8_t>();
        const auto face = in.read<std::uint8_t>();
        const auto i = in.read_varint();
        const auto j = in.read_varint();
        const auto k = in.read_varint();
        const auto size = in.read_varint();
        if (!level || !face || !i || !j || !k || !size) {
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
        volume.coarsest_ = std::max(volume.coarsest_, *level);
        volume.tiles_.emplace(Key{.level = *level,
                                  .face = *face,
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

std::vector<std::byte> TileColumn::encode() const {
    ByteWriter out;
    out.write_varint(floor);
    out.write_varint(tiles.size());
    for (const auto& [k, tree] : tiles) {
        const std::vector<std::byte> bytes = tree.encode();
        out.write_varint(k);
        out.write_varint(bytes.size());
        out.write_bytes(bytes);
    }
    return std::move(out).take();
}

Result<TileColumn> TileColumn::decode(std::span<const std::byte> bytes, int across) {
    const auto fail = [](std::string_view why) {
        return make_error(ErrorCode::InvalidArgument, std::string{why});
    };
    ByteReader in{bytes};
    const auto lowest = in.read_varint();
    const auto count = in.read_varint();
    if (!lowest || !count || *count > bytes.size() || *lowest > UINT32_MAX) {
        return fail("a tile column's bytes are cut short or out of range");
    }
    TileColumn column{.floor = static_cast<std::uint32_t>(*lowest), .tiles = {}};
    for (std::uint64_t n = 0; n < *count; ++n) {
        const auto k = in.read_varint();
        const auto size = in.read_varint();
        if (!k || !size || *k > UINT32_MAX) {
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
        if (tree->side() != across) {
            return fail("a tile column's tile is the wrong size");
        }
        column.tiles.emplace_back(static_cast<std::uint32_t>(*k), std::move(*tree));
    }
    if (in.remaining() != 0) {
        return fail("a tile column's bytes run on past its tiles");
    }
    return column;
}

TileColumn TileColumn::halved() const {
    TileColumn half{.floor = floor / 2, .tiles = {}};
    half.tiles.reserve(tiles.size());
    for (const auto& [k, tree] : tiles) {
        half.tiles.emplace_back(k, globe::halved(tree));
    }
    return half;
}

std::optional<TileColumn> Volume::column(Site site) const {
    const std::optional<std::uint32_t> lowest = floor(site);
    if (!lowest) {
        return std::nullopt;
    }
    const Column at = column_of(site);
    TileColumn column{.floor = *lowest, .tiles = {}};
    for (auto tile = tiles_.lower_bound({.face = at.face, .i = at.i, .j = at.j});
         tile != tiles_.end() && tile->first.level == 0 && tile->first.face == at.face &&
         tile->first.i == at.i && tile->first.j == at.j;
         ++tile) {
        column.tiles.emplace_back(tile->first.k, tile->second);
    }
    return column;
}

std::vector<std::byte> Volume::encode_column(Site site) const {
    const std::optional<TileColumn> found = column(site);
    return found ? found->encode() : std::vector<std::byte>{};
}

Result<void> Volume::decode_column(Site site, std::span<const std::byte> bytes) {
    auto decoded = TileColumn::decode(bytes, tile_side);
    if (!decoded) {
        return std::unexpected(std::move(decoded).error());
    }
    const Column column = column_of(site);
    tiles_.erase(
        tiles_.lower_bound({.face = column.face, .i = column.i, .j = column.j}),
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX}));
    for (auto& [k, tree] : decoded->tiles) {
        tiles_.insert_or_assign({.face = column.face, .i = column.i, .j = column.j, .k = k},
                                std::move(tree));
    }
    if (decoded->tiles.empty()) {
        // Made though all air: its floor's tile is held for it.
        set_floor(site, decoded->floor);
    }
    unsaved_.erase(column);
    return {};
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

std::vector<BrickSite> Volume::changed_since(std::uint64_t revision) const {
    std::set<BrickSite> bricks;
    for (auto entry = log_.rbegin(); entry != log_.rend() && entry->first > revision; ++entry) {
        bricks.insert(entry->second);
    }
    return {bricks.begin(), bricks.end()};
}

void Volume::drop_column(Site site) {
    const Column column = column_of(site);
    unsaved_.erase(column);
    tiles_.erase(
        tiles_.lower_bound({.face = column.face, .i = column.i, .j = column.j}),
        tiles_.upper_bound({.face = column.face, .i = column.i, .j = column.j, .k = UINT32_MAX}));
}

std::vector<Site> Volume::columns() const {
    std::vector<Site> corners;
    for (const auto& [key, tile] : tiles_) {
        if (key.level != 0) {
            break;
        }
        const Site corner{.face = key.face, .i = key.i * side, .j = key.j * side};
        if (corners.empty() || corners.back() != corner) {
            corners.push_back(corner);
        }
    }
    return corners;
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
