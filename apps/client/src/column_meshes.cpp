#include <hog/client/column_meshes.hpp>
#include <hog/globe/column_cache.hpp>

#include <cmath>
#include <utility>

namespace hog::client {
namespace {

// The level the world holds the tile column at; none where it holds none of it.
std::optional<int> level_held(const globe::World& world, globe::Site corner) {
    if (world.volume().floor(corner)) {
        return 0;
    }
    if (const globe::ColumnCache* cache = world.coarse(); cache != nullptr) {
        if (const globe::ColumnCache::Coarse* coarse = cache->coarse(corner)) {
            return coarse->level;
        }
    }
    return std::nullopt;
}

} // namespace

ColumnDrawn drawn_of(const globe::VoxelMesh& mesh, std::optional<double> sea, double thickness) {
    ColumnDrawn drawn{
        .ground = {.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}},
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
            corner = std::abs(stands - *sea) <= thickness ? chunks::globe_sea_corner : 0;
        }
        const auto tile = static_cast<std::uint8_t>(first.material);
        const auto base = static_cast<std::uint32_t>(into.vertices.size());
        for (std::size_t k = 0; k < 4; ++k) {
            into.vertices.push_back({.position = mesh.vertices[face + k].position,
                                     .normal = mesh.vertices[face + k].normal,
                                     .tiles = {tile, tile, tile},
                                     .corner = corner});
        }
        // The face's two triangles, as the mesh has them, onto its corners here.
        const auto from = static_cast<std::uint32_t>(face);
        for (std::size_t k = 0; k < 6; ++k) {
            into.indices.push_back(base + (mesh.indices[(face / 4 * 6) + k] - from));
        }
    }
    return drawn;
}

std::uint64_t column_key(globe::Site corner) noexcept {
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    return (std::uint64_t{corner.face} << 56U) | (std::uint64_t{corner.i / side} << 28U) |
           (corner.j / side);
}

ColumnMeshes::ColumnMeshes(const globe::World& world, jobs::JobSystem& jobs)
    : world_{&world}, jobs_{&jobs} {}

void ColumnMeshes::changed(std::span<const globe::Site> corners) {
    const globe::World* world = world_;
    const std::optional<double> sea =
        world->planet().sea_fraction > 0.0 ? std::optional{world->planet().radius} : std::nullopt;
    const double thickness = world->layers().thickness;
    for (const globe::Site corner : corners) {
        // Meshed as it was, it's out of date.
        std::erase_if(working_, [&](const Work& work) { return work.corner == corner; });
        const std::optional<int> level = level_held(*world, corner);
        if (!level) {
            changes_.gone.push_back(column_key(corner));
            continue;
        }
        working_.push_back(
            {.corner = corner,
             .job = std::make_unique<jobs::Async<ColumnDrawn>>(*jobs_, [=, at = *level] {
                 return drawn_of(globe::mesh_tile_column(*world, corner, at), sea, thickness);
             })});
    }
}

void ColumnMeshes::show(Work& work) {
    ColumnDrawn drawn = work.job->take();
    changes_.shown.push_back(
        {.key = column_key(work.corner),
         .ground = std::make_shared<const terrain::BlockMesh>(std::move(drawn.ground)),
         .water = std::make_shared<const terrain::BlockMesh>(std::move(drawn.water))});
}

void ColumnMeshes::finish() {
    for (Work& work : working_) {
        show(work);
    }
    working_.clear();
}

chunks::GlobeStream::Changes ColumnMeshes::collect() {
    std::erase_if(working_, [&](Work& work) {
        if (!work.job->done()) {
            return false;
        }
        show(work);
        return true;
    });
    return std::exchange(changes_, {});
}

} // namespace hog::client
