#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/physics/ground.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::physics {

namespace {

constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);

// A tile column's ground as the world holds it at its own level, the liquid's surface left out:
// it isn't walked on.
std::optional<Mesh> mesh_of(const globe::World& world, globe::Site corner) {
    const globe::VoxelMesh drawn = globe::mesh_tile_column(world, corner, 0);
    Mesh mesh{.origin = WorldPosition{drawn.origin}, .vertices = {}, .triangles = {}};
    mesh.vertices.reserve(drawn.vertices.size());
    for (const globe::VoxelVertex& vertex : drawn.vertices) {
        mesh.vertices.push_back(vertex.position);
    }
    for (std::size_t corner_at = 0; corner_at + 2 < drawn.indices.size(); corner_at += 3) {
        if (drawn.vertices[drawn.indices[corner_at]].material == globe::Material::Water) {
            continue;
        }
        mesh.triangles.push_back(
            {drawn.indices[corner_at], drawn.indices[corner_at + 1], drawn.indices[corner_at + 2]});
    }
    if (mesh.triangles.empty()) {
        return std::nullopt;
    }
    return mesh;
}

} // namespace

Ground::Ground(Space& space, globe::World world, const GroundSettings& settings)
    : space_(&space), world_(world), settings_(settings) {}

Ground::~Ground() {
    clear();
}

Ground::Ground(Ground&& other) noexcept
    : space_(std::exchange(other.space_, nullptr)), world_(other.world_),
      settings_(other.settings_), columns_(std::move(other.columns_)) {
    other.columns_.clear();
}

Ground& Ground::operator=(Ground&& other) noexcept {
    if (this != &other) {
        clear();
        space_ = std::exchange(other.space_, nullptr);
        world_ = other.world_;
        settings_ = other.settings_;
        columns_ = std::move(other.columns_);
        other.columns_.clear();
    }
    return *this;
}

std::size_t Ground::size() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(columns_, [](const auto& held) { return held.second.has_value(); }));
}

void Ground::let_go(globe::Site corner) noexcept {
    const auto held = columns_.find(corner);
    if (held == columns_.end()) {
        return;
    }
    if (held->second) {
        space_->remove(*held->second);
    }
    columns_.erase(held);
}

void Ground::clear() noexcept {
    if (space_ == nullptr) {
        return;
    }
    for (const auto& [corner, body] : columns_) {
        if (body) {
            space_->remove(*body);
        }
    }
    columns_.clear();
}

Result<> Ground::keep(std::span<const WorldPosition> around) {
    std::set<globe::Site> wanted;
    std::set<globe::Site> near;
    for (const WorldPosition point : around) {
        for (const globe::Site corner :
             globe::tile_columns_near(world_.layers(), point.metres(), settings_.reach)) {
            wanted.insert(corner);
        }
        for (const globe::Site corner :
             globe::tile_columns_near(world_.layers(), point.metres(), 2.0 * settings_.reach)) {
            near.insert(corner);
        }
    }
    std::vector<globe::Site> far;
    for (const auto& [corner, body] : columns_) {
        if (!near.contains(corner)) {
            far.push_back(corner);
        }
    }
    for (const globe::Site corner : far) {
        let_go(corner);
    }
    for (const globe::Site corner : wanted) {
        if (columns_.contains(corner)) {
            continue;
        }
        const std::optional<Mesh> mesh = mesh_of(world_, corner);
        if (!mesh) {
            columns_.emplace(corner, std::nullopt);
            continue;
        }
        auto body = space_->add(*mesh);
        if (!body) {
            return std::unexpected(body.error());
        }
        columns_.emplace(corner, *body);
    }
    return {};
}

void Ground::changed(globe::BrickSite brick) {
    constexpr std::uint32_t across = side / globe::brick_side;
    const globe::Cell column{
        .level = static_cast<std::uint8_t>(world_.layers().level - std::countr_zero(side)),
        .face = brick.face,
        .i = brick.i / across,
        .j = brick.j / across};
    const auto corner_of = [](globe::Cell cell) {
        return globe::Site{.face = cell.face, .i = cell.i * side, .j = cell.j * side};
    };
    let_go(corner_of(column));
    // Its faces on the column's edge are drawn by the column beside it too.
    const auto edge = [](std::uint32_t at) {
        if (at % across == 0) {
            return -1;
        }
        return at % across == across - 1 ? 1 : 0;
    };
    const int di = edge(brick.i);
    const int dj = edge(brick.j);
    for (const auto [step_i, step_j] : {std::pair{di, 0}, std::pair{0, dj}}) {
        if (step_i == 0 && step_j == 0) {
            continue;
        }
        if (const std::optional<globe::Cell> beside = globe::next_to(column, step_i, step_j)) {
            let_go(corner_of(*beside));
        }
    }
}

} // namespace hog::physics
