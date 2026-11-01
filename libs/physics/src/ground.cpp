#include <hog/globe/detail.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/physics/ground.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace hog::physics {

namespace {

// A chunk's ground as drawn, the liquid's surface left out: it isn't walked on.
std::optional<Mesh> mesh_of(const globe::World& world, globe::Cell chunk) {
    globe::SurfaceMesh drawn;
    drawn.origin = globe::center(chunk) * world.generator().planet().radius;
    globe::add_chunk(drawn, world, chunk);
    Mesh mesh{.origin = WorldPosition{drawn.origin}, .vertices = {}, .triangles = {}};
    mesh.vertices.reserve(drawn.vertices.size());
    for (const globe::SurfaceVertex& vertex : drawn.vertices) {
        mesh.vertices.push_back(vertex.position);
    }
    for (std::size_t corner = 0; corner + 2 < drawn.indices.size(); corner += 3) {
        const std::array<std::uint32_t, 3> triangle{
            drawn.indices[corner], drawn.indices[corner + 1], drawn.indices[corner + 2]};
        if (std::ranges::all_of(triangle, [&](std::uint32_t index) {
                return drawn.vertices[index].material == globe::Material::Water;
            })) {
            continue;
        }
        mesh.triangles.push_back(triangle);
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
      settings_(other.settings_), chunks_(std::move(other.chunks_)) {
    other.chunks_.clear();
}

Ground& Ground::operator=(Ground&& other) noexcept {
    if (this != &other) {
        clear();
        space_ = std::exchange(other.space_, nullptr);
        world_ = other.world_;
        settings_ = other.settings_;
        chunks_ = std::move(other.chunks_);
        other.chunks_.clear();
    }
    return *this;
}

void Ground::clear() noexcept {
    if (space_ == nullptr) {
        return;
    }
    for (const auto& [chunk, body] : chunks_) {
        space_->remove(body);
    }
    chunks_.clear();
}

Result<> Ground::keep(std::span<const WorldPosition> around) {
    const double radius = world_.generator().planet().radius;
    std::set<globe::Cell> wanted;
    for (const WorldPosition point : around) {
        for (const globe::Cell chunk :
             globe::chunks_near(world_, glm::normalize(point.metres()), settings_.reach)) {
            wanted.insert(chunk);
        }
    }
    // Chunks farther than twice the reach, and their own width, from every point are let go.
    for (auto held = chunks_.begin(); held != chunks_.end();) {
        const globe::Cell chunk = held->first;
        const std::array<DVec3, 4> corners = globe::corners(chunk);
        const double across = glm::length(corners[1] - corners[0]) * radius;
        const DVec3 middle = globe::center(chunk) * radius;
        const bool near = std::ranges::any_of(around, [&](WorldPosition point) {
            return glm::length((glm::normalize(point.metres()) * radius) - middle) <=
                   (2.0 * settings_.reach) + across;
        });
        if (near || wanted.contains(chunk)) {
            ++held;
            continue;
        }
        space_->remove(held->second);
        held = chunks_.erase(held);
    }
    for (const globe::Cell chunk : wanted) {
        if (chunks_.contains(chunk)) {
            continue;
        }
        const std::optional<Mesh> mesh = mesh_of(world_, chunk);
        if (!mesh) {
            continue;
        }
        auto body = space_->add(*mesh);
        if (!body) {
            return std::unexpected(body.error());
        }
        chunks_.emplace(chunk, *body);
    }
    return {};
}

void Ground::changed(globe::BrickSite brick) {
    // A chunk's mesh reaches a margin of columns into the chunks beside it.
    constexpr std::int64_t margin = 8;
    const auto touches = [&](globe::Cell chunk) {
        if (chunk.face != brick.face) {
            return false;
        }
        const std::int64_t i0 = (std::int64_t{chunk.i} * globe::chunk_side) - margin;
        const std::int64_t j0 = (std::int64_t{chunk.j} * globe::chunk_side) - margin;
        const std::int64_t bi = std::int64_t{brick.i} * globe::brick_side;
        const std::int64_t bj = std::int64_t{brick.j} * globe::brick_side;
        return bi + globe::brick_side > i0 && bi < i0 + globe::chunk_side + (2 * margin) &&
               bj + globe::brick_side > j0 && bj < j0 + globe::chunk_side + (2 * margin);
    };
    std::set<globe::Cell> gone;
    for (const auto& [chunk, body] : chunks_) {
        if (!touches(chunk)) {
            continue;
        }
        gone.insert(chunk);
        const globe::CellNeighbors near = globe::around(chunk);
        for (const globe::Cell next : near.all()) {
            if (next.face != chunk.face && chunks_.contains(next)) {
                gone.insert(next);
            }
        }
    }
    for (const globe::Cell chunk : gone) {
        space_->remove(chunks_.at(chunk));
        chunks_.erase(chunk);
    }
}

} // namespace hog::physics
