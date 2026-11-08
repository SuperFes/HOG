#include <hog/globe/cell.hpp>
#include <hog/globe/voxel_mesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace hog::globe {
namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

// The least share of a voxel a liquid shows in: less wets what's there without standing on it.
constexpr double least_liquid = 1.0 / 16.0;
// The share of a voxel its ground must fill for the voxel to be ground right through.
constexpr double half = 0.5;

// What a voxel holds, as shares of it: its ground, shaped matter and grain together, and liquid.
struct Held {
    Material ground = Material::Air; // the most of its ground
    Material liquid = Material::Air;
    double ground_share = 0.0;
    double liquid_share = 0.0;
};

Held held_of(const Voxel& voxel) {
    constexpr double full = Voxel::full;
    const double solid = voxel.solid == Material::Air ? 0.0 : voxel.solid_amount / full;
    const double grain = voxel.grain == Material::Air ? 0.0 : voxel.grain_amount / full;
    const double liquid = voxel.liquid == Material::Air ? 0.0 : voxel.liquid_amount / full;
    return {.ground = solid >= grain ? voxel.solid : voxel.grain,
            .liquid = voxel.liquid,
            .ground_share = std::min(1.0, solid + grain),
            .liquid_share = std::min(1.0, liquid)};
}

Held held_of(const Cube& cube) {
    return held_of(Voxel{.solid = cube.solid,
                         .grain = cube.grain,
                         .grain_amount = cube.grain_amount,
                         .liquid = cube.liquid,
                         .liquid_amount = cube.liquid_amount,
                         .solid_amount = cube.solid_amount});
}

// Part of a voxel's height drawn as one matter, as shares of it from its bottom.
struct Piece {
    double from = 0.0;
    double to = 0.0;
    Material material = Material::Air;

    [[nodiscard]] bool empty() const noexcept { return to <= from; }
};

// What's drawn in a voxel's place: the ground in it, and the liquid standing over that.
struct Slot {
    Piece ground;
    Piece liquid;
};

// A column voxel by voxel from the bottom, as held and as drawn, and which voxels' ground is drawn
// in another's place.
struct Standing {
    std::span<const Held> column;
    std::vector<Slot> slots;
    std::vector<std::uint8_t> claimed;

    explicit Standing(std::span<const Held> held)
        : column{held}, slots(held.size()), claimed(held.size(), 0) {}

    [[nodiscard]] int count() const noexcept { return static_cast<int>(column.size()); }
    [[nodiscard]] const Held& held(int z) const { return column[static_cast<std::size_t>(z)]; }
    [[nodiscard]] Slot& slot(int z) { return slots[static_cast<std::size_t>(z)]; }
    [[nodiscard]] bool taken(int z) const { return claimed[static_cast<std::size_t>(z)] != 0; }
    void take(int z) { claimed[static_cast<std::size_t>(z)] = 1; }
    [[nodiscard]] bool partial(int z) const {
        const double share = held(z).ground_share;
        return share > 0.0 && share < half;
    }
};

// The ground hanging under the ground from `first` up, as low as the matter no ground under it
// stands up into makes.
void hang(Standing& standing, int first) {
    double under = 0.0;
    for (int k = first - 1; k >= 0 && !standing.taken(k) && standing.partial(k); --k) {
        under += standing.held(k).ground_share;
        standing.take(k);
    }
    for (int k = first - 1; k >= 0 && under > 0.0; --k) {
        standing.slot(k).ground = {
            .from = 1.0 - std::min(1.0, under), .to = 1.0, .material = standing.held(first).ground};
        under -= 1.0;
    }
}

// Ground is wherever a voxel is at least half of it, and the ground beside that stands as high, or
// hangs as low, as all of it in the voxels over, or under, it makes.
void stand_ground(Standing& standing) {
    const int count = standing.count();
    for (int z = 0; z < count;) {
        if (standing.held(z).ground_share < half) {
            ++z;
            continue;
        }
        const int first = z;
        for (; z < count && standing.held(z).ground_share >= half; ++z) {
            standing.slot(z).ground = {.from = 0.0, .to = 1.0, .material = standing.held(z).ground};
            standing.take(z);
        }
        const int last = z - 1;
        double over = standing.held(last).ground_share;
        for (; z < count && standing.partial(z); ++z) {
            over += standing.held(z).ground_share;
            standing.take(z);
        }
        const Material top = standing.held(last).ground;
        for (int k = last; k < count && over > 0.0; ++k) {
            const Material own = standing.held(k).ground;
            standing.slot(k).ground = {.from = 0.0,
                                       .to = std::min(1.0, over),
                                       .material = own != Material::Air ? own : top};
            over -= 1.0;
        }
        hang(standing, first);
    }
}

// Liquid stands over the ground as high as all of it makes, with the ground no ground stands on
// lying in it. What's in the pores of ground drawn right through stays in it, unseen.
void stand_liquid(Standing& standing) {
    const int count = standing.count();
    const auto open = [&](int z) {
        const Piece& ground = standing.slot(z).ground;
        return standing.held(z).liquid_share > 0.0 &&
               (ground.empty() || ground.from > 0.0 || ground.to < 1.0);
    };
    for (int z = 0; z < count;) {
        if (!open(z)) {
            ++z;
            continue;
        }
        const int first = z;
        const Material material = standing.held(z).liquid;
        double total = 0.0;
        for (; z < count && open(z); ++z) {
            total += standing.held(z).liquid_share +
                     (standing.taken(z) ? 0.0 : standing.held(z).ground_share);
        }
        if (total < least_liquid) {
            continue;
        }
        for (int k = first; k < count && total > 0.0; ++k) {
            const Piece& ground = standing.slot(k).ground;
            const double bottom = ground.empty() ? 0.0 : ground.to;
            if (bottom >= 1.0 && k != first) {
                break;
            }
            const double part = std::min(1.0 - bottom, total);
            standing.slot(k).liquid = {.from = bottom, .to = bottom + part, .material = material};
            total -= part;
        }
    }
}

// Where a column's matter stands, voxel by voxel from the bottom.
std::vector<Slot> stood(std::span<const Held> column) {
    Standing standing{column};
    stand_ground(standing);
    stand_liquid(standing);
    return std::move(standing.slots);
}

// The voxels of a column and the columns about it, a voxel past it all round, as drawn.
class Block {
public:
    Block(int across, int lowest, int height)
        : across_{across}, lowest_{lowest}, height_{height},
          slots_(static_cast<std::size_t>((across + 2) * (across + 2) * height)) {}

    [[nodiscard]] int across() const noexcept { return across_; }
    [[nodiscard]] int lowest() const noexcept { return lowest_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    // `x` and `y` from -1 to across, `z` from lowest up height of them; open space past those.
    [[nodiscard]] const Slot& at(int x, int y, int z) const {
        static const Slot open;
        return z < lowest_ || z >= lowest_ + height_ ? open : slots_[index(x, y, z)];
    }
    // Draws the column at `x`, `y` as it holds what's in it, from the lowest up.
    void stand(int x, int y, std::span<const Held> column) {
        const std::vector<Slot> drawn = stood(column);
        for (int z = 0; z < height_; ++z) {
            slots_[index(x, y, lowest_ + z)] = drawn[static_cast<std::size_t>(z)];
        }
    }

private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const {
        const int wide = across_ + 2;
        const int flat = ((((z - lowest_) * wide) + (y + 1)) * wide) + (x + 1);
        return static_cast<std::size_t>(flat);
    }

    int across_;
    int lowest_;
    int height_;
    std::vector<Slot> slots_;
};

// The layers the tile column holds, as voxels of the level: from under its floor to over its
// highest tile.
struct Span {
    int lowest = 0;
    int highest = 0; // past the last
};

std::optional<Span> span_of(const World& world, Site corner, int level) {
    const int size = 1 << level;
    std::optional<std::uint32_t> floor;
    std::uint32_t top = 0;
    if (level == 0) {
        floor = world.volume().floor(corner);
        top = world.volume().held_top(corner);
    } else if (const ColumnCache* cache = world.coarse(); cache != nullptr) {
        if (const ColumnCache::Coarse* coarse = cache->coarse(corner);
            coarse != nullptr && std::cmp_equal(coarse->level, level)) {
            floor = coarse->column.floor << static_cast<unsigned>(level);
            top = coarse->column.tiles.empty() ? *floor
                                               : (coarse->column.tiles.back().first + 1) * side;
        }
    }
    if (!floor) {
        return std::nullopt;
    }
    const int count = world.layers().count / size;
    const auto highest = static_cast<int>(std::max(top, *floor));
    return Span{.lowest = static_cast<int>(*floor) / size,
                .highest = std::min(count, (highest + size - 1) / size)};
}

void quad(VoxelMesh& mesh, const std::array<DVec3, 8>& box, const std::array<std::size_t, 4>& at,
          Material material) {
    std::array<DVec3, 4> points{};
    DVec3 face{0.0};
    DVec3 middle{0.0};
    for (const DVec3& corner : box) {
        middle += corner;
    }
    middle /= 8.0;
    for (std::size_t k = 0; k < 4; ++k) {
        points.at(k) = box.at(at.at(k));
        face += points.at(k);
    }
    face /= 4.0;
    DVec3 normal = glm::cross(points[1] - points[0], points[2] - points[0]);
    const bool flip = glm::dot(normal, face - middle) < 0.0;
    if (flip) {
        normal = -normal;
    }
    const double length = glm::length(normal);
    const Vec3 unit = length > 0.0 ? Vec3{normal / length} : Vec3{0.0F, 0.0F, 1.0F};
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const DVec3& point : points) {
        mesh.vertices.push_back(
            {.position = Vec3{point - mesh.origin}, .normal = unit, .material = material});
    }
    for (const std::uint32_t corner : flip ? std::array<std::uint32_t, 6>{0, 2, 1, 0, 3, 2}
                                           : std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3}) {
        mesh.indices.push_back(first + corner);
    }
}

// Reads the column whose cells at `first`'s level start at it, and those beside it, into the
// block: a cell of the world's own level in the middle of each stands for it.
void fill(const World& world, Block& block, Cell first, int size) {
    const Layers& layers = world.layers();
    const int across = block.across();
    const int bottom = std::max(block.lowest(), 0);
    const int top = std::min(block.lowest() + block.height(), layers.count / size);
    for (int y = -1; y <= across; ++y) {
        for (int x = -1; x <= across; ++x) {
            // Those past its corners are never looked at.
            const bool beside = (x >= 0 && x < across) || (y >= 0 && y < across);
            const std::optional<Cell> column = beside ? next_to(first, x, y) : std::nullopt;
            if (!column) {
                continue;
            }
            const auto middle = static_cast<std::uint32_t>(size / 2);
            const Cell fine{.level = static_cast<std::uint8_t>(layers.level),
                            .face = column->face,
                            .i = (column->i * static_cast<std::uint32_t>(size)) + middle,
                            .j = (column->j * static_cast<std::uint32_t>(size)) + middle};
            const std::vector<Voxel> voxels =
                world.fill(std::span{&fine, 1}, bottom * size, top - bottom, size);
            std::vector<Held> held(static_cast<std::size_t>(block.height()));
            for (int z = block.lowest(); z < block.lowest() + block.height(); ++z) {
                Held& here = held[static_cast<std::size_t>(z - block.lowest())];
                if (z < bottom) {
                    here = {.ground = world.planet().high.rock, .ground_share = 1.0};
                } else if (z < top) {
                    here = held_of(voxels[static_cast<std::size_t>(z - bottom)]);
                }
            }
            block.stand(x, y, held);
        }
    }
}

// The box of the voxel `x`, `y` along the cells from `first`, `low` to `high` metres from the
// planet's centre: its column's four corners at its bottom (0 to 3) and top (4 to 7).
std::array<DVec3, 8> box_of(Cell first, int x, int y, double low, double high) {
    const std::array<DVec3, 4> rim = corners(Cell{.level = first.level,
                                                  .face = first.face,
                                                  .i = first.i + static_cast<std::uint32_t>(x),
                                                  .j = first.j + static_cast<std::uint32_t>(y)});
    // The cell's corners run round it; the box's by bits: 1 across, 2 along.
    constexpr std::array<std::size_t, 4> round{0, 1, 3, 2};
    std::array<DVec3, 8> box{};
    for (std::size_t k = 0; k < 4; ++k) {
        box.at(k) = rim.at(round.at(k)) * low;
        box.at(k + 4) = rim.at(round.at(k)) * high;
    }
    return box;
}

// Each side of a box: its axis, which way, and the four box corners it is.
struct Side {
    int axis = 0;
    int by = 0;
    std::array<std::size_t, 4> corners{};
};
constexpr std::array<Side, 6> box_sides{{{.axis = 0, .by = -1, .corners = {0, 2, 6, 4}},
                                         {.axis = 0, .by = 1, .corners = {1, 5, 7, 3}},
                                         {.axis = 1, .by = -1, .corners = {0, 4, 5, 1}},
                                         {.axis = 1, .by = 1, .corners = {2, 3, 7, 6}},
                                         {.axis = 2, .by = -1, .corners = {0, 1, 3, 2}},
                                         {.axis = 2, .by = 1, .corners = {4, 6, 7, 5}}}};

// What of `piece` isn't behind `hiding`, at most two parts of it.
std::array<Piece, 2> uncovered(Piece piece, std::span<const Piece> hiding) {
    std::array<Piece, 2> left{piece, Piece{}};
    for (const Piece& cover : hiding) {
        if (cover.empty()) {
            continue;
        }
        std::array<Piece, 2> cut{};
        std::size_t kept = 0;
        for (const Piece& part : left) {
            if (part.empty()) {
                continue;
            }
            if (cover.to <= part.from || cover.from >= part.to) {
                cut.at(kept++) = part;
                continue;
            }
            if (part.from < cover.from && kept < cut.size()) {
                cut.at(kept++) = {.from = part.from, .to = cover.from, .material = part.material};
            }
            if (cover.to < part.to && kept < cut.size()) {
                cut.at(kept++) = {.from = cover.to, .to = part.to, .material = part.material};
            }
        }
        left = cut;
    }
    return left;
}

// Whether a piece reaches the bottom of its voxel, or the top.
bool from_bottom(const Piece& piece) {
    return !piece.empty() && piece.from <= 0.0;
}
bool to_top(const Piece& piece) {
    return !piece.empty() && piece.to >= 1.0;
}

// Whether the top of `piece`, `by` up, or its bottom is hidden by `next` over or under it. Ground
// shows against all but ground; a liquid against open space.
bool end_hidden(const Piece& piece, bool liquid, const Slot& here, const Slot& next, int by) {
    if (by > 0) {
        return to_top(piece) && (from_bottom(next.ground) || (liquid && from_bottom(next.liquid)));
    }
    const bool resting = liquid && !here.ground.empty() && here.ground.to >= piece.from;
    return resting ||
           (from_bottom(piece) && (to_top(next.ground) || (liquid && to_top(next.liquid))));
}

// What of `piece` in the voxel at `x`, `y`, `z` of the block shows on its side `face`.
std::array<Piece, 2> shown(const Block& block, int x, int y, int z, const Piece& piece, bool liquid,
                           const Side& face) {
    const Slot& here = block.at(x, y, z);
    if (face.axis == 2) {
        const Slot& next = block.at(x, y, z + face.by);
        return end_hidden(piece, liquid, here, next, face.by) ? std::array<Piece, 2>{}
                                                              : std::array{piece, Piece{}};
    }
    const Slot& beside =
        block.at(x + (face.axis == 0 ? face.by : 0), y + (face.axis == 1 ? face.by : 0), z);
    const std::array<Piece, 2> hiding{beside.ground, liquid ? beside.liquid : Piece{}};
    return uncovered(piece, hiding);
}

// The faces that show of what's drawn in the voxel at `x`, `y`, `z` of the block, `low` to `high`
// metres from the planet's centre.
void add_voxel(VoxelMesh& mesh, const Block& block, Cell first, int x, int y, int z, double low,
               double high) {
    const Slot& here = block.at(x, y, z);
    const auto metres = [&](double share) { return low + (share * (high - low)); };
    for (const bool liquid : {false, true}) {
        const Piece& piece = liquid ? here.liquid : here.ground;
        if (piece.empty()) {
            continue;
        }
        for (const Side& face : box_sides) {
            for (const Piece& part : shown(block, x, y, z, piece, liquid, face)) {
                if (!part.empty()) {
                    quad(mesh, box_of(first, x, y, metres(part.from), metres(part.to)),
                         face.corners, part.material);
                }
            }
        }
    }
}

// The cube at (x, y, z) from the tile's corner, a voxel past it at most: from the tile beside
// where `beside` has it, else from the tile's own nearest edge.
Cube cube_near(TileKey key, const VoxelTree& tile, const TileAt& beside, int x, int y, int z) {
    const int across = Volume::tile_side;
    const auto step = [&](int at) {
        if (at < 0) {
            return -1;
        }
        return at >= across ? 1 : 0;
    };
    const int dx = step(x);
    const int dy = step(y);
    const int dz = step(z);
    if (dx == 0 && dy == 0 && dz == 0) {
        return tile.at(x, y, z);
    }
    const auto moved = [](std::uint32_t at, int by) -> std::optional<std::uint32_t> {
        if (by < 0 && at == 0) {
            return std::nullopt;
        }
        return by < 0 ? at - 1 : at + static_cast<std::uint32_t>(by);
    };
    const auto i = moved(key.i, dx);
    const auto j = moved(key.j, dy);
    const auto k = moved(key.k, dz);
    if (beside && i && j && k) {
        if (const VoxelTree* next =
                beside({.level = key.level, .face = key.face, .i = *i, .j = *j, .k = *k});
            next != nullptr) {
            return next->at(x - (dx * across), y - (dy * across), z - (dz * across));
        }
    }
    return tile.at(std::clamp(x, 0, across - 1), std::clamp(y, 0, across - 1),
                   std::clamp(z, 0, across - 1));
}

} // namespace

VoxelMesh mesh_tile(const Layers& layers, TileKey key, const VoxelTree& tile,
                    const TileAt& beside) {
    VoxelMesh mesh;
    constexpr int across = Volume::tile_side;
    if (tile.leaf_side(0, 0, 0) == across && tile.at(0, 0, 0) == Cube{}) {
        return mesh;
    }
    const int size = 1 << key.level;
    const int base = static_cast<int>(key.k) * across;
    const int top = std::min(base + across, (layers.count + size - 1) / size);
    if (top <= base) {
        return mesh;
    }
    const Cell first{.level = static_cast<std::uint8_t>(layers.level - key.level),
                     .face = key.face,
                     .i = key.i * static_cast<std::uint32_t>(across),
                     .j = key.j * static_cast<std::uint32_t>(across)};
    const Cell whole{
        .level = static_cast<std::uint8_t>(layers.level - key.level - std::countr_zero(side)),
        .face = key.face,
        .i = key.i,
        .j = key.j};
    mesh.origin = center(whole) * layers.bottom(base * size);

    Block block{across, base - 1, across + 2};
    std::vector<Held> column(static_cast<std::size_t>(across + 2));
    for (int y = -1; y <= across; ++y) {
        for (int x = -1; x <= across; ++x) {
            // Those past its corners are never looked at.
            if ((x < 0 || x >= across) && (y < 0 || y >= across)) {
                continue;
            }
            for (std::size_t n = 0; n < column.size(); ++n) {
                const int z = static_cast<int>(n) - 1;
                column[n] =
                    base + z < top ? held_of(cube_near(key, tile, beside, x, y, z)) : Held{};
            }
            block.stand(x, y, column);
        }
    }
    for (int z = base; z < top; ++z) {
        const double low = layers.bottom(z * size);
        const double high = layers.bottom((z + 1) * size);
        for (int y = 0; y < across; ++y) {
            for (int x = 0; x < across; ++x) {
                add_voxel(mesh, block, first, x, y, z, low, high);
            }
        }
    }
    return mesh;
}

VoxelMesh mesh_tile_column(const World& world, Site corner, int level) {
    const Layers& layers = world.layers();
    const std::optional<Span> span = span_of(world, corner, level);
    VoxelMesh mesh;
    if (!span || span->highest <= span->lowest) {
        return mesh;
    }
    const int size = 1 << level;
    const int across = Volume::tile_side >> level;
    const Cell first{.level = static_cast<std::uint8_t>(layers.level - level),
                     .face = corner.face,
                     .i = (corner.i / side) * static_cast<std::uint32_t>(across),
                     .j = (corner.j / side) * static_cast<std::uint32_t>(across)};
    const Cell tile{.level = static_cast<std::uint8_t>(layers.level - std::countr_zero(side)),
                    .face = corner.face,
                    .i = corner.i / side,
                    .j = corner.j / side};
    mesh.origin = center(tile) * layers.bottom(span->lowest * size);

    // A voxel under the floor, all rock, and one over the top, all air.
    Block block{across, span->lowest - 1, span->highest - span->lowest + 2};
    fill(world, block, first, size);
    for (int z = span->lowest; z < span->highest; ++z) {
        const double low = layers.bottom(z * size);
        const double high = layers.bottom((z + 1) * size);
        for (int y = 0; y < across; ++y) {
            for (int x = 0; x < across; ++x) {
                add_voxel(mesh, block, first, x, y, z, low, high);
            }
        }
    }
    return mesh;
}

} // namespace hog::globe
