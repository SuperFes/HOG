#include <hog/planet/projection.hpp>
#include <hog/planet/volume_cell.hpp>
#include <hog/terrain/blocks.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    Builder(const planet::Volume& volume, double radius, planet::CellId region)
        : volume_{&volume}, radius_{radius} {
        mesh_.origin = planet::surface_position(region, radius);
    }

    // Adds the sides that show of the part of a leaf in the region.
    void add(const planet::Volume::Leaf& leaf, VolumeCell part) {
        const auto around = planet::face_neighbors(part);
        for (std::size_t index = 0; index < around.size(); ++index) {
            if (around[index].is_valid()) {
                add_side(leaf, static_cast<Side>(index), {.here = part, .across = around[index]});
            }
        }
    }

    [[nodiscard]] BlockMesh take() && { return std::move(mesh_); }

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
        const auto first = static_cast<std::uint32_t>(mesh_.vertices.size());
        for (const DVec3& corner : corners) {
            mesh_.vertices.push_back({.position = relative_to(WorldPosition{corner}, mesh_.origin),
                                      .normal = Vec3{normal},
                                      .colour = block_colour(material)});
        }
        for (const std::uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
            mesh_.indices.push_back(first + corner);
        }
    }

    const planet::Volume* volume_;
    double radius_;
    BlockMesh mesh_;
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

BlockMesh block_mesh(const planet::Volume& volume, double radius, planet::CellId region) {
    Builder builder{volume, radius, region};
    volume.for_each_leaf_in(region, [&](const planet::Volume::Leaf& leaf, VolumeCell part) {
        builder.add(leaf, part);
    });
    return std::move(builder).take();
}

} // namespace hog::terrain
