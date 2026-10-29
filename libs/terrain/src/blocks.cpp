#include <hog/planet/geology.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/volume_cell.hpp>
#include <hog/terrain/blocks.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::terrain {

namespace {

using planet::Material;
using planet::Side;
using planet::VolumeCell;

// How much can be seen through a material; a side shows where it faces something clearer.
[[nodiscard]] int clarity(Material material) {
    switch (material) {
    case Material::Air:
        return 2;
    case Material::Water:
        return 1;
    default:
        return 0;
    }
}

// What a leaf shows as in a part of it.
[[nodiscard]] Material shown(const planet::Volume::Leaf& leaf, VolumeCell part, double radius) {
    return planet::material_at(leaf, radius, planet::volume_center(part, radius));
}

// A side's corners, in no particular winding.
[[nodiscard]] std::array<DVec3, 4> side_corners(VolumeCell cell, Side side, double radius) {
    const auto corners = planet::cell_vertices(cell.column());
    const planet::ShellBounds bounds = planet::shell_bounds(cell, radius);
    if (side == Side::Down || side == Side::Up) {
        const double reach = side == Side::Down ? bounds.inner : bounds.outer;
        return {corners[0] * reach, corners[1] * reach, corners[2] * reach, corners[3] * reach};
    }
    // Corners run u0v0, u1v0, u1v1, u0v1.
    constexpr std::array<std::array<std::size_t, 2>, 4> edges{{{0, 3}, {1, 2}, {0, 1}, {3, 2}}};
    const auto [a, b] = edges[static_cast<std::size_t>(side)];
    return {corners[a] * bounds.inner, corners[b] * bounds.inner, corners[b] * bounds.outer,
            corners[a] * bounds.outer};
}

class Builder {
public:
    Builder(BlockMesh& mesh, const planet::Volume& volume, double radius)
        : mesh_{&mesh}, volume_{&volume}, radius_{radius} {}

    // Adds the sides that show of the part of a leaf in the region.
    void add(const planet::Volume::Leaf& leaf, VolumeCell part) {
        const auto around = planet::face_neighbors(part);
        // Nothing lies past the planet's centre to see a side from.
        const bool at_centre = planet::shell_bounds(part, radius_).inner <= 0.0;
        for (std::size_t index = 0; index < around.size(); ++index) {
            if (around[index].is_valid() &&
                (!at_centre || static_cast<Side>(index) != Side::Down)) {
                add_side(leaf, static_cast<Side>(index), {.here = part, .across = around[index]});
            }
        }
    }

private:
    // Part of a leaf's side and what lies across it.
    struct Piece {
        VolumeCell here;
        VolumeCell across;
    };

    // Each leaf shows its own side wherever it faces something clearer, piece by piece where the
    // cells across are split finer, so every side between two leaves is shown once.
    void add_side(const planet::Volume::Leaf& leaf, Side side, Piece whole) {
        std::vector<Piece> pending{whole};
        while (!pending.empty()) {
            const Piece piece = pending.back();
            pending.pop_back();
            const auto held = volume_->leaf_holding(piece.across);
            if (!held.has_value()) {
                split(piece, side, volume_->splits_across(piece.across), pending);
                continue;
            }
            const Material here = shown(leaf, piece.here, radius_);
            const Material there =
                shown(held.value_or(planet::Volume::Leaf{}), piece.across, radius_);
            if (clarity(there) > clarity(here)) {
                quad(piece.here, side, here);
            }
        }
    }

    // Splits a piece the way the cells across it split.
    static void split(Piece piece, Side side, bool across, std::vector<Piece>& pending) {
        const bool vertical = side == Side::Down || side == Side::Up;
        if (across) {
            for (int index = 0; index < 4; ++index) {
                const VolumeCell here = piece.here.across_child(index);
                // Only the children along a side face its cells across.
                const VolumeCell facing =
                    vertical ? piece.across.across_child(index)
                             : planet::face_neighbors(here)[static_cast<std::size_t>(side)];
                if (vertical || piece.across.contains(facing)) {
                    pending.push_back({.here = here, .across = facing});
                }
            }
        } else if (vertical) {
            // Of the cells over or under, only the nearer half touches.
            pending.push_back(
                {.here = piece.here, .across = piece.across.shell_child(side == Side::Up ? 0 : 1)});
        } else {
            for (int index = 0; index < 2; ++index) {
                pending.push_back({.here = piece.here.shell_child(index),
                                   .across = piece.across.shell_child(index)});
            }
        }
    }

    // A cell's side, facing out of it.
    void quad(VolumeCell cell, Side side, Material material) {
        auto corners = side_corners(cell, side, radius_);
        const DVec3 middle = (corners[0] + corners[1] + corners[2] + corners[3]) / 4.0;
        const DVec3 away = middle - planet::volume_center(cell, radius_).metres();
        DVec3 normal = glm::normalize(glm::cross(corners[1] - corners[0], corners[2] - corners[0]));
        if (glm::dot(normal, away) < 0.0) {
            std::swap(corners[1], corners[3]);
            normal = -normal;
        }
        const auto first = static_cast<std::uint32_t>(mesh_->vertices.size());
        for (const DVec3& corner : corners) {
            mesh_->vertices.push_back(
                {.position = relative_to(WorldPosition{corner}, mesh_->origin),
                 .normal = Vec3{normal},
                 .colour = block_colour(material)});
        }
        for (const std::uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
            mesh_->indices.push_back(first + corner);
        }
    }

    BlockMesh* mesh_;
    const planet::Volume* volume_;
    double radius_;
};

} // namespace

Vec3 block_colour(Material material) {
    switch (material) {
    case Material::Air:
        return {0.8F, 0.9F, 1.0F};
    case Material::Water:
        return {0.05F, 0.2F, 0.45F};
    case Material::Snow:
        return {0.9F, 0.92F, 0.95F};
    case Material::Ice:
        return {0.6F, 0.8F, 0.9F};
    case Material::Soil:
        return {0.3F, 0.22F, 0.12F};
    case Material::Sediment:
        return {0.6F, 0.52F, 0.36F};
    case Material::Sandstone:
        return {0.7F, 0.5F, 0.3F};
    case Material::Mudstone:
        return {0.4F, 0.38F, 0.35F};
    case Material::Limestone:
        return {0.75F, 0.73F, 0.65F};
    case Material::Granite:
        return {0.55F, 0.5F, 0.48F};
    case Material::Basalt:
        return {0.18F, 0.18F, 0.2F};
    case Material::Metamorphic:
        return {0.45F, 0.42F, 0.5F};
    case Material::Magma:
        return {1.0F, 0.35F, 0.05F};
    }
    return {1.0F, 0.0F, 1.0F};
}

void add_blocks(BlockMesh& mesh, const planet::Volume& volume, double radius,
                planet::CellId region) {
    Builder builder{mesh, volume, radius};
    volume.for_each_leaf_in(region, [&](const planet::Volume::Leaf& leaf, VolumeCell part) {
        builder.add(leaf, part);
    });
}

BlockMesh block_mesh(const planet::Volume& volume, double radius, planet::CellId region) {
    BlockMesh mesh{
        .origin = planet::surface_position(region, radius), .vertices = {}, .indices = {}};
    add_blocks(mesh, volume, radius, region);
    return mesh;
}

namespace {

// Shells `height` metres tall, or the nearest finer.
int shell_level_for(double height) {
    int shell_level = 0;
    while (shell_level < planet::max_shell_level &&
           std::ldexp(planet::volume_span, -shell_level) > height) {
        ++shell_level;
    }
    return shell_level;
}

double shell_height(int shell_level) {
    return std::ldexp(planet::volume_span, -shell_level);
}

// The terrain's height over the planet's radius in the middle of `column`.
double ground_in(const planet::World& world, planet::CellId column) {
    return static_cast<double>(world.geology().terrain().elevation(column)) /
           static_cast<double>(planet::fixed_one);
}

constexpr float stand_in_temperature = 288.0F; // kelvin

// Fills `column` from `low` to `high` metres from the centre: rock up to `ground` metres over the
// radius, the sea over it, and air above.
void fill_column(planet::Volume& volume, double radius, planet::CellId column, double ground,
                 double low, double high, int shell_level) {
    const double surface = radius + ground;
    const double sea = std::max(surface, radius);
    planet::fill_between(volume, radius, column, low, surface, shell_level,
                         {.material = Material::Granite,
                          .temperature = stand_in_temperature,
                          .surface = std::nullopt});
    planet::fill_between(volume, radius, column, surface, sea, shell_level,
                         {.material = Material::Water,
                          .temperature = stand_in_temperature,
                          .surface = std::nullopt});
    planet::fill_between(
        volume, radius, column, sea, high, shell_level,
        {.material = Material::Air, .temperature = stand_in_temperature, .surface = std::nullopt});
}

// A ring of columns at `level` within `reach`, in shells at `shell_level`.
struct Ring {
    int level = 0;
    double reach = 0.0;
    int shell_level = 0;
};

// From the whole planet under them in, each finer and nearer.
std::vector<Ring> rings_for(double radius) {
    std::vector<Ring> rings{{.level = overview_level,
                             .reach = std::numeric_limits<double>::infinity(),
                             .shell_level = shell_level_for(overview_shell)}};
    double size = block_size;
    double reach = block_reach;
    std::vector<Ring> inner;
    for (int level = planet::level_for(block_size, radius); level > overview_level;
         level -= ring_step) {
        // Shells a quarter of a column across, though no coarser than the planet's.
        inner.push_back(
            {.level = level,
             .reach = reach,
             .shell_level = shell_level_for(std::min(size, 4.0 * overview_shell) / 4.0)});
        const double across = std::ldexp(1.0, ring_step);
        size *= across;
        reach *= across;
    }
    rings.insert(rings.end(), inner.rbegin(), inner.rend());
    return rings;
}

// Fills in `ring` round the unit direction `around` over what `coarser` filled in, between the
// heights either has.
void fill_ring(planet::Volume& volume, const planet::World& world, double radius, DVec3 around,
               const Ring& ring, const Ring& coarser) {
    const planet::CellId middle = planet::cell_at(around, ring.level);
    std::vector<planet::CellId> columns{middle};
    std::set<planet::CellId> seen{middle};
    // Past what either rounded its heights to.
    const double margin = shell_height(coarser.shell_level) + shell_height(ring.shell_level);
    for (std::size_t next = 0; next < columns.size(); ++next) {
        const planet::CellId column = columns[next];
        const double ground = ground_in(world, column);
        const double under = ground_in(world, column.parent(coarser.level));
        fill_column(volume, radius, column, ground, radius + std::min(ground, under) - margin,
                    radius + std::max({ground, under, 0.0}) + margin, ring.shell_level);
        for (const planet::CellId beside : planet::edge_neighbors(column)) {
            if (glm::length(planet::cell_center(beside) - around) * radius <= ring.reach &&
                seen.insert(beside).second) {
                columns.push_back(beside);
            }
        }
    }
}

} // namespace

planet::Volume planet_volume(const planet::World& world, double radius) {
    const Ring whole = rings_for(radius).front();
    planet::Volume volume{
        {.material = Material::Air, .temperature = stand_in_temperature, .surface = std::nullopt},
        overview_level - 2};
    const double bottom = radius - (planet::volume_span / 2.0);
    const std::uint32_t across = 1U << static_cast<unsigned>(overview_level);
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t i = 0; i < across; ++i) {
            for (std::uint32_t j = 0; j < across; ++j) {
                const planet::CellId column =
                    planet::CellId::from_face_ij(face, i, j, overview_level);
                const double ground = ground_in(world, column);
                fill_column(volume, radius, column, ground, bottom, radius + std::max(ground, 0.0),
                            whole.shell_level);
            }
        }
    }
    volume.settle();
    return volume;
}

BlockMesh ground_blocks(const planet::Volume& planet, const planet::World& world, DVec3 around,
                        double radius) {
    planet::Volume volume = planet;
    const std::vector<Ring> rings = rings_for(radius);
    for (std::size_t ring = 1; ring < rings.size(); ++ring) {
        fill_ring(volume, world, radius, around, rings[ring], rings[ring - 1]);
    }
    volume.settle();
    BlockMesh mesh{
        .origin = planet::surface_position(planet::cell_at(around, rings.back().level), radius),
        .vertices = {},
        .indices = {}};
    const int region_level = volume.region_level();
    const std::uint32_t across = 1U << static_cast<unsigned>(region_level);
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t i = 0; i < across; ++i) {
            for (std::uint32_t j = 0; j < across; ++j) {
                add_blocks(mesh, volume, radius,
                           planet::CellId::from_face_ij(face, i, j, region_level));
            }
        }
    }
    return mesh;
}

BlockMesh ground_blocks(const planet::World& world, DVec3 around, double radius) {
    return ground_blocks(planet_volume(world, radius), world, around, radius);
}

void BlockStream::follow(DVec3 eye) {
    if (!wanted_ || glm::length(eye - *wanted_) * radius_ > block_relay) {
        wanted_ = eye;
    }
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

void BlockStream::start(DVec3 around) {
    started_ = around;
    working_ = std::make_unique<jobs::Async<Filled>>(*jobs_, [world = world_, around,
                                                              radius = radius_, planet = planet_] {
        std::shared_ptr<const planet::Volume> under =
            planet ? planet : std::make_shared<const planet::Volume>(planet_volume(*world, radius));
        BlockMesh mesh = ground_blocks(*under, *world, around, radius);
        return Filled{.mesh = std::move(mesh), .planet = std::move(under)};
    });
}

BlockMesh BlockStream::take() {
    Filled filled = working_->take();
    working_.reset();
    planet_ = std::move(filled.planet);
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return std::move(filled.mesh);
}

std::optional<BlockMesh> BlockStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    return take();
}

std::optional<BlockMesh> BlockStream::finish() {
    std::optional<BlockMesh> last;
    while (working_) {
        last = take();
    }
    return last;
}

} // namespace hog::terrain
