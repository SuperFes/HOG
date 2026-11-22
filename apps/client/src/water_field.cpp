#include <hog/client/drawn.hpp>
#include <hog/client/water_field.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>
#include <vector>

namespace hog::client {
namespace {

// A dry point's value: over any water, so an edge to it never crosses a surface.
constexpr double dry = 1.0;

// How a crossing carries its water to water_of: how far it rises and when in its first material, a
// byte each, which of the field's bodies it is in its second, and a share under nothing where the
// water only ends.
constexpr float water_ends = -1.0F;

// How far along from `a` to `b` the segment meets the sphere of `radius` about the centre, where
// one end lies inside it and the other not.
double meets_sphere(DVec3 a, DVec3 b, double radius) noexcept {
    const DVec3 run = b - a;
    const double qa = glm::dot(run, run);
    const double qb = 2.0 * glm::dot(a, run);
    const double qc = glm::dot(a, a) - (radius * radius);
    const double root = std::sqrt(std::max(0.0, (qb * qb) - (4.0 * qa * qc)));
    // Of the two, the one between the ends: entering from inside the larger, leaving the smaller.
    const double t = qc < 0.0 ? (-qb + root) / (2.0 * qa) : (-qb - root) / (2.0 * qa);
    return std::clamp(t, 0.0, 1.0);
}

} // namespace

WaterField::WaterField(const PiecesSnapshot& pieces, const grid::WaterBodies& bodies)
    : pieces_{&pieces}, bodies_{&bodies}, grid_{pieces.grid()}, origin_{grid_.low(grid::Key{})},
      across_{std::int64_t{1} << grid_.depth()} {}

DVec3 WaterField::at(I64Vec3 point) const noexcept {
    return origin_ + (DVec3{point} * grid_.finest());
}

const WaterField::Leaf& WaterField::leaf(I64Vec3 point) {
    for (const Leaf* seen : recent_) {
        if (seen != nullptr && glm::all(glm::lessThanEqual(seen->low, point)) &&
            glm::all(glm::lessThan(point, seen->high))) {
            return *seen;
        }
    }
    const grid::Key node{.depth = grid_.depth(),
                         .x = static_cast<std::uint32_t>(point.x),
                         .y = static_cast<std::uint32_t>(point.y),
                         .z = static_cast<std::uint32_t>(point.z)};
    const auto [found, made] = leaves_.try_emplace(grid::code(node));
    if (made) {
        Leaf& made_leaf = found->second;
        const auto known = pieces_->at<grid::Water>(node);
        const grid::Key over = known ? node.ancestor(known->found.depth) : grid::Key{};
        made_leaf.body = known ? known->found.value : 0;
        const auto shift = static_cast<unsigned>(grid_.depth() - over.depth);
        made_leaf.low = I64Vec3{over.x, over.y, over.z} * (std::int64_t{1} << shift);
        made_leaf.high = made_leaf.low + I64Vec3{std::int64_t{1} << shift};
    }
    recent_[next_recent_] = &found->second;
    next_recent_ = (next_recent_ + 1) % recent_.size();
    return found->second;
}

const std::optional<WaterField::Wet>& WaterField::wet(I64Vec3 point) {
    const auto key = (static_cast<std::uint64_t>(point.x) << 42U) |
                     (static_cast<std::uint64_t>(point.y) << 21U) |
                     static_cast<std::uint64_t>(point.z);
    const auto [found, made] = wets_.try_emplace(key);
    if (!made) {
        return found->second;
    }
    grid::Water::Value lowest = 0;
    for (unsigned octant = 0; octant < 8; ++octant) {
        const I64Vec3 node =
            point - I64Vec3{(octant & 1U) == 0U ? 1 : 0, (octant & 2U) == 0U ? 1 : 0,
                            (octant & 4U) == 0U ? 1 : 0};
        if (glm::any(glm::lessThan(node, I64Vec3{0})) ||
            glm::any(glm::greaterThanEqual(node, I64Vec3{across_}))) {
            continue;
        }
        const Leaf& near = leaf(node);
        if (near.body != 0 && (lowest == 0 || near.body < lowest) &&
            bodies_->bodies.contains(near.body)) {
            lowest = near.body;
        }
        // Inside one leaf, every node round the point lies in it.
        if (glm::all(glm::lessThan(near.low, point)) && glm::all(glm::lessThan(point, near.high))) {
            break;
        }
    }
    if (lowest == 0) {
        return found->second;
    }
    const auto [seen, first] =
        seen_.try_emplace(lowest, static_cast<std::uint16_t>(seen_.size() + 1));
    if (first) {
        seen_bodies_.push_back(&bodies_->bodies.at(lowest));
    }
    const grid::Body& body = *seen_bodies_[seen->second - 1];
    if (const auto* sea = std::get_if<grid::Sea>(&body)) {
        found->second = Wet{.level = sea->level, .body = seen->second};
    } else if (const auto* lake = std::get_if<grid::Lake>(&body)) {
        found->second = Wet{.level = lake->level,
                            .rise = lake->season.rise,
                            .peak = lake->season.peak,
                            .body = seen->second};
    } else if (const auto& river = std::get<grid::River>(body); river.course.size() >= 2) {
        const grid::River::Along along = river.along(at(point));
        found->second =
            Wet{.level = along.level, .rise = along.rise, .peak = river.peak, .body = seen->second};
    }
    return found->second;
}

double WaterField::value(I64Vec3 point) {
    const std::optional<Wet>& water = wet(point);
    return water ? glm::length(at(point)) - water->level : dry;
}

Crossed WaterField::crossing(I64Vec3 low, I64Vec3 high) {
    const std::optional<Wet> from = wet(low);
    const std::optional<Wet> to = wet(high);
    const DVec3 a = at(low);
    const DVec3 b = at(high);
    // A dry end over the wet one's level is air over the water, dry only for lying in nodes wholly
    // over it; the water ends where a dry end is under its level, at the ground.
    bool ends = !from && !to;
    if (from && !to) {
        ends = glm::length(b) <= from->level;
    } else if (!from && to) {
        ends = glm::length(a) <= to->level;
    }
    if (ends) {
        const DVec3 end = from ? a : b;
        return {
            .at = end, .normal = Vec3{glm::normalize(end)}, .materials = {}, .share = water_ends};
    }
    const Wet& wet_a = from ? *from : *to;
    const Wet& wet_b = to ? *to : *from;
    const double over_a = glm::length(a) - wet_a.level;
    const double over_b = glm::length(b) - wet_b.level;
    const double t = wet_a.level == wet_b.level ? meets_sphere(a, b, wet_a.level)
                                                : std::clamp(over_a / (over_a - over_b), 0.0, 1.0);
    const DVec3 crossed = a + ((b - a) * t);
    const Wet& under = over_a < 0.0 ? wet_a : wet_b;
    return {.at = crossed,
            .normal = Vec3{glm::normalize(crossed)},
            .materials = {static_cast<std::uint16_t>(globe::rise_of(under.rise) |
                                                     (globe::peak_of(under.peak) << 8U)),
                          under.body},
            .share = 1.0F};
}

bool WaterField::sea(std::uint16_t body) const noexcept {
    return body > 0 && std::holds_alternative<grid::Sea>(*seen_bodies_[body - 1]);
}

DVec3 WaterField::current(std::uint16_t body, DVec3 place) const noexcept {
    const auto* river = body > 0 ? std::get_if<grid::River>(seen_bodies_[body - 1]) : nullptr;
    if (river == nullptr || river->course.size() < 2) {
        return DVec3{0.0};
    }
    const grid::River::Along along = river->along(place);
    // Along the ground, however its course falls.
    const DVec3 up = glm::normalize(place);
    const DVec3 level = along.downstream - (up * glm::dot(along.downstream, up));
    const double length = glm::length(level);
    return length > 0.0 ? level * (grid::running_speed(along.flow) / length) : DVec3{0.0};
}

BlockField WaterField::field() {
    return {.value = [this](I64Vec3 point) { return value(point); },
            .crossing = [this](I64Vec3 low, I64Vec3 high) { return crossing(low, high); }};
}

bool may_hold_water(const PiecesSnapshot& pieces, grid::Key node) {
    // A node's value is the most common body under it but none, so none means none at all.
    const auto known = pieces.at<grid::Water>(node);
    return known && known->found.value != 0;
}

std::optional<globe::StandingWater> still_water_in(const HeldPieces& held, DVec3 eye) {
    const std::optional<grid::Key> node = held.grid().key_at(eye, held.grid().depth());
    const auto known = node ? held.at<grid::Water>(*node) : std::nullopt;
    if (!known || known->found.value == 0) {
        return std::nullopt;
    }
    const auto found = held.bodies().bodies.find(known->found.value);
    if (found == held.bodies().bodies.end()) {
        return std::nullopt;
    }
    if (const auto* lake = std::get_if<grid::Lake>(&found->second)) {
        return globe::StandingWater{
            .top = lake->level, .rise = lake->season.rise, .peak = lake->season.peak};
    }
    if (const auto* river = std::get_if<grid::River>(&found->second);
        river != nullptr && river->course.size() >= 2) {
        const grid::River::Along along = river->along(eye);
        return globe::StandingWater{.top = along.level, .rise = along.rise, .peak = river->peak};
    }
    return std::nullopt;
}

terrain::BlockMesh water_of(const ShapeMesh& mesh, const WaterField& field) {
    terrain::BlockMesh water{.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}};
    std::vector<bool> edge(mesh.vertices.size(), false);
    std::vector<std::uint32_t> kept;
    kept.reserve(mesh.indices.size());
    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        const std::array<std::uint32_t, 3> triangle{mesh.indices[corner], mesh.indices[corner + 1],
                                                    mesh.indices[corner + 2]};
        const bool ends = std::ranges::any_of(
            triangle, [&](std::uint32_t at) { return mesh.vertices[at].share < 0.0F; });
        // Crossings met at a lattice point leave triangles of no size.
        const auto& [a, b, c] = triangle;
        if (mesh.vertices[a].position == mesh.vertices[b].position ||
            mesh.vertices[b].position == mesh.vertices[c].position ||
            mesh.vertices[c].position == mesh.vertices[a].position) {
            continue;
        }
        if (!ends) {
            kept.insert(kept.end(), triangle.begin(), triangle.end());
            continue;
        }
        for (const std::uint32_t at : triangle) {
            edge[at] = true;
        }
    }
    constexpr std::uint32_t unused = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> index(mesh.vertices.size(), unused);
    water.indices.reserve(kept.size());
    for (const std::uint32_t at : kept) {
        if (index[at] == unused) {
            const ShapeVertex& vertex = mesh.vertices[at];
            const std::uint16_t body = vertex.materials[1];
            const bool sea = field.sea(body);
            index[at] = static_cast<std::uint32_t>(water.vertices.size());
            water.vertices.push_back(
                {.position = vertex.position,
                 .normal = Vec3{field.current(body, mesh.origin + DVec3{vertex.position})},
                 .tiles = {static_cast<std::uint8_t>(globe::Material::Water),
                           static_cast<std::uint8_t>(vertex.materials[0] & 0xFFU),
                           static_cast<std::uint8_t>(vertex.materials[0] >> 8U)},
                 .corner = static_cast<std::uint8_t>((sea ? sea_corner : 0U) |
                                                     (edge[at] ? shore_corner : 0U))});
        }
        water.indices.push_back(index[at]);
    }
    return water;
}

} // namespace hog::client
