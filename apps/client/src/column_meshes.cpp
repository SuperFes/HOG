#include <hog/client/column_meshes.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace hog::client {
namespace {

// The level the world holds the tile column at; none where it holds none of it.
std::optional<int> level_held(const globe::World& world, globe::Site corner) {
    if (world.volume().floor(corner)) {
        return 0;
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
            corner = std::abs(stands - *sea) <= thickness ? sea_corner : 0;
        }
        const auto tile = static_cast<std::uint8_t>(first.material);
        const auto base = static_cast<std::uint32_t>(into.vertices.size());
        for (std::size_t k = 0; k < 4; ++k) {
            const globe::VoxelVertex& vertex = mesh.vertices[face + k];
            into.vertices.push_back({.position = vertex.position,
                                     .normal = vertex.normal,
                                     .tiles = {tile, tile, tile},
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

std::uint64_t column_key(globe::Site corner) noexcept {
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    return (std::uint64_t{corner.face} << 56U) | (std::uint64_t{corner.i / side} << 28U) |
           (corner.j / side);
}

ColumnMeshes::ColumnMeshes(const globe::World& world, jobs::JobSystem& jobs)
    : world_{&world}, jobs_{&jobs} {}

void ColumnMeshes::changed(std::span<const globe::Site> corners) {
    const globe::World* world = world_;
    // A formed world's sea stands at the radius wherever its volatile can lie liquid.
    const std::optional<double> sea = globe::derive(world->planet()).liquid
                                          ? std::optional{world->planet().body.radius}
                                          : std::nullopt;
    const double voxel = world->layers().thickness;
    // Those beside each, meshed, show their sides against it as it was.
    std::vector<globe::Site> all{corners.begin(), corners.end()};
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    constexpr int coarser = std::countr_zero(side);
    const auto columns = static_cast<std::uint8_t>(world->layers().level - coarser);
    for (const globe::Site corner : corners) {
        const globe::Cell cell{
            .level = columns, .face = corner.face, .i = corner.i / side, .j = corner.j / side};
        for (const auto [di, dj] :
             {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
            if (const std::optional<globe::Cell> next = globe::next_to(cell, di, dj)) {
                const globe::Site beside{
                    .face = next->face, .i = next->i * side, .j = next->j * side};
                if (meshed_.contains(beside)) {
                    all.push_back(beside);
                }
            }
        }
    }
    std::ranges::sort(all);
    const auto [repeated, end] = std::ranges::unique(all);
    all.erase(repeated, end);
    for (const globe::Site corner : all) {
        // Meshed as it was, it's out of date.
        std::erase_if(working_, [&](const Work& work) { return work.corner == corner; });
        const std::optional<int> level = level_held(*world, corner);
        if (!level) {
            let_go(corner);
            continue;
        }
        meshed_.insert(corner);
        working_.push_back(
            {.corner = corner,
             .job = std::make_unique<jobs::Async<ColumnDrawn>>(*jobs_, [=, at = *level] {
                 // Coarser, each voxel stands for 2^level a side.
                 return drawn_of(globe::mesh_tile_column(*world, corner, at), sea,
                                 std::ldexp(voxel, at));
             })});
    }
}

void ColumnMeshes::let_go(globe::Site corner) {
    changes_.gone.push_back(column_key(corner));
    meshed_.erase(corner);
    if (drawn_.erase(corner) != 0 && around_) {
        around_->drawn = 0;
    }
}

void ColumnMeshes::show(Work& work) {
    drawn_.insert(work.corner);
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

void ColumnMeshes::follow(DVec3 eye, double metres) {
    const std::vector<globe::Site> near = globe::tile_columns_near(world_->layers(), eye, metres);
    std::vector<globe::Site> changed;
    for (const globe::Site corner : near) {
        if (!meshed_.contains(corner) && level_held(*world_, corner)) {
            changed.push_back(corner);
        }
    }
    const auto far = [&](const globe::Site corner) {
        return !std::ranges::binary_search(near, corner);
    };
    std::vector<globe::Site> left;
    std::ranges::copy_if(meshed_, std::back_inserter(left), far);
    for (const globe::Site corner : left) {
        let_go(corner);
    }
    std::erase_if(working_, [&](const Work& work) { return far(work.corner); });
    this->changed(changed);
}

double ColumnMeshes::covered(DVec3 eye, double metres) {
    const globe::Layers& layers = world_->layers();
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    constexpr int coarser = std::countr_zero(side);
    const auto level = static_cast<std::uint8_t>(layers.level - coarser);
    // No point of a tile column lies further than this from its middle.
    const double across = side * layers.thickness;
    // Measured again only once the eye's a tile column off, as that takes milliseconds.
    if (!around_ || around_->metres != metres || glm::length(eye - around_->from) > across) {
        around_ = Around{.from = eye, .metres = metres, .columns = {}, .drawn = 0};
        const double radius = glm::length(eye);
        for (const globe::Site corner : globe::tile_columns_near(layers, eye, metres)) {
            const globe::Cell cell{
                .level = level, .face = corner.face, .i = corner.i / side, .j = corner.j / side};
            around_->columns.emplace_back(glm::length((globe::center(cell) * radius) - eye),
                                          corner);
        }
        std::ranges::sort(around_->columns);
    }
    Around& around = *around_;
    while (around.drawn < around.columns.size() &&
           drawn_.contains(around.columns[around.drawn].second)) {
        ++around.drawn;
    }
    if (around.drawn == around.columns.size()) {
        return metres;
    }
    const double slack = across + glm::length(eye - around.from);
    return std::clamp(around.columns[around.drawn].first - slack, 0.0, metres);
}

GroundChanges ColumnMeshes::collect() {
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
