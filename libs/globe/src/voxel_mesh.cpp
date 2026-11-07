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

// What a voxel is as drawn: its shaped matter, else grain that fills it, else a liquid that does;
// open space where none does.
struct Kind {
    Material material = Material::Air;
    bool liquid = false;
};

Kind kind_of(const Voxel& voxel) {
    const auto half = static_cast<std::uint16_t>(Voxel::full / 2);
    if (voxel.solid != Material::Air && voxel.solid_amount >= half) {
        return {.material = voxel.solid};
    }
    if (voxel.grain != Material::Air && voxel.grain_amount >= half) {
        return {.material = voxel.grain};
    }
    if (voxel.liquid != Material::Air && voxel.liquid_amount >= half) {
        return {.material = voxel.liquid, .liquid = true};
    }
    return {};
}

// The voxels of a column and the columns about it, a voxel past it all round.
class Block {
public:
    Block(int across, int lowest, int height)
        : across_{across}, lowest_{lowest}, height_{height},
          kinds_(static_cast<std::size_t>((across + 2) * (across + 2) * height)) {}

    [[nodiscard]] int across() const noexcept { return across_; }
    [[nodiscard]] int lowest() const noexcept { return lowest_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    // `x` and `y` from -1 to across, `z` from lowest up height of them.
    [[nodiscard]] Kind& at(int x, int y, int z) { return kinds_[index(x, y, z)]; }
    [[nodiscard]] const Kind& at(int x, int y, int z) const { return kinds_[index(x, y, z)]; }

private:
    [[nodiscard]] std::size_t index(int x, int y, int z) const {
        const int wide = across_ + 2;
        const int flat = ((((z - lowest_) * wide) + (y + 1)) * wide) + (x + 1);
        return static_cast<std::size_t>(flat);
    }

    int across_;
    int lowest_;
    int height_;
    std::vector<Kind> kinds_;
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
            const auto half = static_cast<std::uint32_t>(size / 2);
            const Cell fine{.level = static_cast<std::uint8_t>(layers.level),
                            .face = column->face,
                            .i = (column->i * static_cast<std::uint32_t>(size)) + half,
                            .j = (column->j * static_cast<std::uint32_t>(size)) + half};
            const std::vector<Voxel> voxels =
                world.fill(std::span{&fine, 1}, bottom * size, top - bottom, size);
            for (int z = block.lowest(); z < bottom; ++z) {
                block.at(x, y, z) = {.material = world.planet().high.rock};
            }
            for (int z = bottom; z < top; ++z) {
                block.at(x, y, z) = kind_of(voxels[static_cast<std::size_t>(z - bottom)]);
            }
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

// The faces of the voxel at `x`, `y`, `z` of the block that show.
void add_voxel(VoxelMesh& mesh, const Block& block, Cell first, int x, int y, int z, double low,
               double high) {
    const Kind here = block.at(x, y, z);
    if (here.material == Material::Air) {
        return;
    }
    std::optional<std::array<DVec3, 8>> box;
    for (const Side& face : box_sides) {
        const Kind beside =
            block.at(x + (face.axis == 0 ? face.by : 0), y + (face.axis == 1 ? face.by : 0),
                     z + (face.axis == 2 ? face.by : 0));
        // Solid shows against anything not solid; a liquid only against open space.
        const bool open = here.liquid ? beside.material == Material::Air
                                      : beside.material == Material::Air || beside.liquid;
        if (!open) {
            continue;
        }
        if (!box) {
            box = box_of(first, x, y, low, high);
        }
        quad(mesh, *box, face.corners, here.material);
    }
}

} // namespace

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
