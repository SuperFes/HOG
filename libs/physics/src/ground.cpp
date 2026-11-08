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

// A tile's ground as the world holds it, the tiles beside it read from the world, the liquid's
// surface left out: it isn't walked on.
std::optional<Mesh> mesh_of(const globe::World& world, globe::TileKey key,
                            const globe::VoxelTree& tile) {
    const globe::VoxelMesh drawn = globe::mesh_tile(
        world.layers(), key, tile, [&](globe::TileKey at) { return world.tile(at); });
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
      settings_(other.settings_), tiles_(std::move(other.tiles_)) {
    other.tiles_.clear();
}

Ground& Ground::operator=(Ground&& other) noexcept {
    if (this != &other) {
        clear();
        space_ = std::exchange(other.space_, nullptr);
        world_ = other.world_;
        settings_ = other.settings_;
        tiles_ = std::move(other.tiles_);
        other.tiles_.clear();
    }
    return *this;
}

std::size_t Ground::size() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(tiles_, [](const auto& held) { return held.second.has_value(); }));
}

void Ground::let_go(globe::TileKey key) noexcept {
    const auto held = tiles_.find(key);
    if (held == tiles_.end()) {
        return;
    }
    if (held->second) {
        space_->remove(*held->second);
    }
    tiles_.erase(held);
}

void Ground::clear() noexcept {
    if (space_ == nullptr) {
        return;
    }
    for (const auto& [key, body] : tiles_) {
        if (body) {
            space_->remove(*body);
        }
    }
    tiles_.clear();
}

std::set<globe::TileKey> Ground::near(std::span<const WorldPosition> around, double reach) const {
    const globe::Layers& layers = world_.layers();
    std::set<globe::TileKey> found;
    for (const WorldPosition point : around) {
        const double radius = glm::length(point.metres());
        // Ground within reach across can stand further up or down than that on a steep slope.
        const auto tile_of = [&](double at) {
            return static_cast<std::uint32_t>(std::clamp(layers.layer_at(at), 0, layers.count - 1) /
                                              globe::Volume::tile_side);
        };
        const std::uint32_t lowest = tile_of(radius - (2.0 * reach));
        const std::uint32_t highest = tile_of(radius + (2.0 * reach));
        for (const globe::Site corner : globe::tile_columns_near(layers, point.metres(), reach)) {
            for (std::uint32_t k = lowest; k <= highest; ++k) {
                const globe::TileKey finest{.level = 0,
                                            .face = corner.face,
                                            .i = corner.i / side,
                                            .j = corner.j / side,
                                            .k = k};
                const std::optional<globe::HeldTile> over = world_.tile_over(finest);
                found.insert(over ? over->key : finest);
            }
        }
    }
    return found;
}

Result<> Ground::keep(std::span<const WorldPosition> around) {
    const std::set<globe::TileKey> wanted = near(around, settings_.reach);
    const std::set<globe::TileKey> close = near(around, 2.0 * settings_.reach);
    std::vector<globe::TileKey> far;
    for (const auto& [key, body] : tiles_) {
        if (!close.contains(key)) {
            far.push_back(key);
        }
    }
    for (const globe::TileKey key : far) {
        let_go(key);
    }
    for (const globe::TileKey key : wanted) {
        if (tiles_.contains(key)) {
            continue;
        }
        const globe::VoxelTree* tile = world_.tile(key);
        const std::optional<Mesh> mesh =
            tile != nullptr ? mesh_of(world_, key, *tile) : std::nullopt;
        if (!mesh) {
            tiles_.emplace(key, std::nullopt);
            continue;
        }
        auto body = space_->add(*mesh);
        if (!body) {
            return std::unexpected(body.error());
        }
        tiles_.emplace(key, *body);
    }
    return {};
}

void Ground::changed(globe::BrickSite brick) {
    constexpr std::uint32_t bricks = side / globe::brick_side;
    changed(globe::TileKey{.level = 0,
                           .face = brick.face,
                           .i = brick.i / bricks,
                           .j = brick.j / bricks,
                           .k = brick.layer / bricks});
}

void Ground::changed(globe::TileKey key) {
    std::vector<globe::TileKey> touched;
    for (const auto& [kept, body] : tiles_) {
        if (globe::meets(kept, key)) {
            touched.push_back(kept);
        }
    }
    for (const globe::TileKey kept : touched) {
        let_go(kept);
    }
}

bool Ground::holds(globe::TileKey key) const {
    for (int up = 0; up < 32 && key.level < UINT8_MAX; ++up, key = key.parent()) {
        if (tiles_.contains(key)) {
            return true;
        }
    }
    return false;
}

} // namespace hog::physics
