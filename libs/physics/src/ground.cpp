#include <hog/physics/ground.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <utility>

namespace hog::physics {

namespace {

// A tile's ground as drawn when its neighbours are as fine as it is, skirts left out.
Mesh mesh_of(const terrain::Patch& patch) {
    Mesh mesh{.origin = patch.origin, .vertices = {}, .triangles = {}};
    mesh.vertices.reserve(terrain::patch_grid_vertices);
    for (int vertex = 0; vertex < terrain::patch_grid_vertices; ++vertex) {
        mesh.vertices.push_back(patch.vertices[static_cast<std::size_t>(vertex)].position);
    }
    const auto indices = terrain::patch_indices(0);
    mesh.triangles.reserve(indices.size() / 3);
    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        mesh.triangles.push_back({indices[corner], indices[corner + 1], indices[corner + 2]});
    }
    return mesh;
}

// Metres across a cell at a level, on average.
double across(int level, double radius) {
    return std::sqrt(4.0 * std::numbers::pi * radius * radius / 6.0) / std::ldexp(1.0, level);
}

} // namespace

Ground::Ground(Space& space, const planet::World& world, const GroundSettings& settings)
    : space_(&space), world_(&world), settings_(settings),
      level_(std::clamp(planet::level_for(settings.tile, world.geology().planet_radius()), 0,
                        terrain::max_patch_level)),
      across_(across(level_, world.geology().planet_radius())) {}

Ground::~Ground() {
    clear();
}

Ground::Ground(Ground&& other) noexcept
    : space_(std::exchange(other.space_, nullptr)), world_(other.world_),
      settings_(other.settings_), level_(other.level_), across_(other.across_),
      tiles_(std::move(other.tiles_)) {
    other.tiles_.clear();
}

Ground& Ground::operator=(Ground&& other) noexcept {
    if (this != &other) {
        clear();
        space_ = std::exchange(other.space_, nullptr);
        world_ = other.world_;
        settings_ = other.settings_;
        level_ = other.level_;
        across_ = other.across_;
        tiles_ = std::move(other.tiles_);
        other.tiles_.clear();
    }
    return *this;
}

void Ground::clear() noexcept {
    if (space_ == nullptr) {
        return;
    }
    for (const auto& [tile, body] : tiles_) {
        space_->remove(body);
    }
    tiles_.clear();
}

Result<> Ground::keep(std::span<const WorldPosition> around) {
    const double radius = world_->geology().planet_radius();
    // Tiles are found by sampling a square around each point more finely than the smallest is
    // across, which also finds those over a cube face's edge.
    std::set<planet::CellId> wanted;
    const double step = across_ / 3.0;
    const int steps = static_cast<int>(std::ceil(settings_.reach / step));
    for (const WorldPosition point : around) {
        const planet::TangentFrame frame = planet::tangent_frame(point);
        for (int y = -steps; y <= steps; ++y) {
            for (int x = -steps; x <= steps; ++x) {
                const DVec3 offset = frame.to_world({x * step, y * step, 0.0});
                wanted.insert(planet::cell_at(point + offset, level_));
            }
        }
    }

    const double let_go = (2.0 * settings_.reach) + across_;
    for (auto tile = tiles_.begin(); tile != tiles_.end();) {
        const WorldPosition centre = planet::surface_position(tile->first, radius);
        const bool near = std::ranges::any_of(around, [&](WorldPosition point) {
            const DVec3 up = glm::normalize(point.metres());
            return glm::length((up * radius) - centre.metres()) <= let_go;
        });
        if (near || wanted.contains(tile->first)) {
            ++tile;
            continue;
        }
        space_->remove(tile->second);
        tile = tiles_.erase(tile);
    }

    for (const planet::CellId tile : wanted) {
        if (tiles_.contains(tile)) {
            continue;
        }
        auto body = space_->add(mesh_of(terrain::bake_patch(*world_, tile)));
        if (!body) {
            return std::unexpected(body.error());
        }
        tiles_.emplace(tile, *body);
    }
    return {};
}

} // namespace hog::physics
