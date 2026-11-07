#include <hog/globe/detail.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/physics/ground.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::physics {

namespace {

// A chunk's ground as drawn, seeing as far under it as `band` reaches, the liquid's surface left
// out: it isn't walked on.
std::optional<Mesh> mesh_of(const globe::World& world, globe::Cell chunk, std::uint64_t band) {
    globe::SurfaceMesh drawn;
    drawn.origin = globe::center(chunk) * world.planet().radius;
    globe::add_chunk(drawn, world, chunk, globe::Drawn{}, globe::sight_of(band));
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

std::size_t Ground::size() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        chunks_, [](const auto& held) { return held.second.body.has_value(); }));
}

void Ground::drop(const Held& held) noexcept {
    if (held.body) {
        space_->remove(*held.body);
    }
}

void Ground::clear() noexcept {
    if (space_ == nullptr) {
        return;
    }
    for (const auto& [chunk, held] : chunks_) {
        drop(held);
    }
    chunks_.clear();
}

Result<> Ground::keep(std::span<const WorldPosition> around) {
    const double radius = world_.planet().radius;
    std::set<globe::Cell> wanted;
    for (const WorldPosition point : around) {
        const DVec3 direction = glm::normalize(point.metres());
        // Any chunk that lies within reach, not only one whose middle does: where a chunk is
        // wider than the reach, the one under the feet may have its middle farther than that.
        const globe::Cell under = globe::chunk_under(direction, world_.layers());
        const double half_across =
            glm::length(globe::corners(under)[0] - globe::center(under)) * radius;
        for (const globe::Cell chunk :
             globe::chunks_near(world_, direction, settings_.reach + half_across)) {
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
        drop(held->second);
        held = chunks_.erase(held);
    }
    for (const globe::Cell chunk : wanted) {
        // Seeing under the ground as far as the point nearest it does.
        const DVec3 middle = globe::center(chunk) * radius;
        const WorldPosition nearest = *std::ranges::min_element(
            around, {}, [&](WorldPosition point) { return glm::length(point.metres() - middle); });
        const double at = glm::length(nearest.metres());
        if (const auto held = chunks_.find(chunk); held != chunks_.end()) {
            if (globe::sight_holds(held->second.band, at)) {
                continue;
            }
            drop(held->second);
            chunks_.erase(held);
        }
        const std::uint64_t band = globe::sight_band(at);
        const std::optional<Mesh> mesh = mesh_of(world_, chunk, band);
        if (!mesh) {
            chunks_.emplace(chunk, Held{.body = std::nullopt, .band = band});
            continue;
        }
        auto body = space_->add(*mesh);
        if (!body) {
            return std::unexpected(body.error());
        }
        chunks_.emplace(chunk, Held{.body = *body, .band = band});
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
    for (const auto& [chunk, held] : chunks_) {
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
        drop(chunks_.at(chunk));
        chunks_.erase(chunk);
    }
}

} // namespace hog::physics
