#include <hog/client/drawn.hpp>
#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hog::client {
namespace {

// A face's tiles at a corner: its own, or for water, which draws one, how it rises through the
// year in the other two.
std::array<std::uint8_t, 3> tiles_of(const globe::VoxelVertex& vertex, std::uint8_t tile,
                                     bool liquid) {
    if (liquid) {
        return {tile, vertex.rise, vertex.peak};
    }
    return {tile, tile, tile};
}

} // namespace

Drawn drawn_of(const globe::VoxelMesh& mesh, std::optional<double> sea, double thickness) {
    Drawn drawn{.ground = {.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}},
                .water = {.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}}};
    // Faces are four corners each, a voxel's matter all through.
    for (std::size_t face = 0; face + 3 < mesh.vertices.size(); face += 4) {
        const globe::VoxelVertex& first = mesh.vertices[face];
        const bool liquid = first.material == globe::Material::Water;
        terrain::BlockMesh& into = liquid ? drawn.water : drawn.ground;
        std::uint8_t corner = 0;
        if (liquid && sea) {
            double stands = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                stands += glm::length(mesh.origin + DVec3{mesh.vertices[face + k].position}) / 4.0;
            }
            corner = std::abs(stands - *sea) <= thickness ? sea_corner : 0;
        }
        const auto tile = static_cast<std::uint8_t>(first.material);
        const auto base = static_cast<std::uint32_t>(into.vertices.size());
        for (std::size_t k = 0; k < 4; ++k) {
            const globe::VoxelVertex& vertex = mesh.vertices[face + k];
            // The sea stands at its one level, however much water each voxel at it holds.
            const Vec3 position =
                (corner & sea_corner) != 0
                    ? Vec3{(glm::normalize(mesh.origin + DVec3{vertex.position}) * sea.value()) -
                           mesh.origin}
                    : vertex.position;
            into.vertices.push_back({.position = position,
                                     .normal = vertex.normal,
                                     .tiles = tiles_of(vertex, tile, liquid),
                                     .corner = static_cast<std::uint8_t>(
                                         corner | (liquid && vertex.shore ? shore_corner : 0U))});
        }
        // The face's two triangles, as the mesh has them, onto its corners here.
        const auto from = static_cast<std::uint32_t>(face);
        for (std::size_t k = 0; k < 6; ++k) {
            into.indices.push_back(base + (mesh.indices[(face / 4 * 6) + k] - from));
        }
    }
    return drawn;
}

std::optional<globe::StandingWater> still_water_at(const globe::World& world, DVec3 eye,
                                                   std::optional<double> sea, double thickness) {
    const std::optional<globe::StandingWater> water = globe::water_around(world, eye);
    if (!water || (sea && std::abs(water->top - *sea) <= thickness)) {
        return std::nullopt;
    }
    return water;
}

} // namespace hog::client
