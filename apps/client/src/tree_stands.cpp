#include <hog/client/tree_stands.hpp>
#include <hog/core/hash.hpp>
#include <hog/grid/kinds.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hog::client {
namespace {

// Where a line leaves a box, along its direction from its start.
std::pair<double, double> through_box(DVec3 from, DVec3 direction, DVec3 low, double side) {
    double near = -std::numeric_limits<double>::infinity();
    double far = std::numeric_limits<double>::infinity();
    for (glm::length_t i = 0; i < 3; ++i) {
        if (direction[i] == 0.0) {
            if (from[i] < low[i] || from[i] >= low[i] + side) {
                return {0.0, 0.0};
            }
            continue;
        }
        const double a = (low[i] - from[i]) / direction[i];
        const double b = (low[i] + side - from[i]) / direction[i];
        near = std::max(near, std::min(a, b));
        far = std::min(far, std::max(a, b));
    }
    return {near, far};
}

} // namespace

std::optional<GroundHit> ground_along(const PiecesSnapshot& pieces, DVec3 from, DVec3 to) {
    const grid::Grid& grid = pieces.grid();
    const double length = glm::length(to - from);
    if (length <= 0.0) {
        return std::nullopt;
    }
    const DVec3 direction = (to - from) / length;
    // Past a leaf's far side by this, to land in the next.
    const double nudge = grid.finest() * 1e-6;
    bool air_behind = false;
    double t = 0.0;
    while (t < length) {
        const DVec3 point = from + (direction * t);
        const auto node = grid.key_at(point, grid.depth());
        const auto known = node ? pieces.at<grid::Shape>(*node) : std::nullopt;
        if (!known) {
            return std::nullopt;
        }
        const grid::Key leaf = node->ancestor(known->found.depth);
        const double side = grid.size(leaf.depth);
        const double leaves =
            std::min(through_box(from, direction, grid.low(leaf), side).second, length);
        const grid::Surface surface = known->found.value;
        DVec3 normal = -direction;
        if (surface.crosses()) {
            normal = surface.normal();
            const DVec3 centre = grid.centre(leaf);
            const double out = surface.distance() * side;
            const auto height = [&](double at) {
                return out + glm::dot(normal, from + (direction * at) - centre);
            };
            // Neighbouring leaves' planes needn't meet, so a step from air into ground at their
            // seam counts too.
            if (air_behind && height(t) <= 0.0) {
                return GroundHit{.at = point, .normal = normal};
            }
            const double facing = glm::dot(normal, direction);
            if (facing < 0.0) {
                const double hit = std::max(t, -height(0.0) / facing);
                if (hit < leaves && height(t) > 0.0) {
                    return GroundHit{.at = from + (direction * hit), .normal = normal};
                }
            }
            air_behind = height(leaves) > 0.0;
        } else {
            if (air_behind && surface.is_solid()) {
                return GroundHit{.at = point, .normal = normal};
            }
            air_behind = surface.is_open();
        }
        t = std::max(leaves, t) + nudge;
    }
    return std::nullopt;
}

std::uint8_t woods_depth(const grid::Grid& grid) noexcept {
    std::uint8_t depth = grid.depth();
    while (depth > 0 && grid.size(depth) < grid::Woods::finest) {
        --depth;
    }
    return depth;
}

std::vector<StandingTree> trees_in(const PiecesSnapshot& pieces, grid::Key node, std::uint64_t seed,
                                   const flora::Planting& planting) {
    const auto known = pieces.at<grid::Woods>(node);
    if (!known || !known->found.value.ground) {
        return {};
    }
    const grid::Stand& stand = known->found.value;
    flora::Woods woods{.cover = stand.cover_share()};
    for (std::size_t kind = 0; kind < flora::kinds; ++kind) {
        woods.shares.at(kind) = stand.share(kind);
    }
    const grid::Grid& grid = pieces.grid();
    const DVec3 middle = grid.centre(node);
    const DVec3 up = glm::normalize(middle);
    const DVec3 low = grid.low(node);
    const double side = grid.size(node.depth);
    std::vector<StandingTree> out;
    for (const flora::Spot& spot :
         flora::spots(middle, side, up, hash_values(seed, grid::code(node)), planting.spacing)) {
        const auto tree = flora::plant(spot, woods, planting);
        if (!tree) {
            continue;
        }
        const auto [bottom, top] = through_box(spot.at, up, low, side);
        if (bottom >= top) {
            continue;
        }
        // Starting just within the node, so nothing need be held over it.
        const double within = side * 1e-9;
        const auto ground =
            ground_along(pieces, spot.at + (up * (top - within)), spot.at + (up * bottom));
        if (ground && glm::dot(ground->normal, up) >= steepest_stand) {
            out.push_back({.at = ground->at, .tree = *tree});
        }
    }
    return out;
}

} // namespace hog::client
