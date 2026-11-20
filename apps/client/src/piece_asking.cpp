#include <hog/client/piece_asking.hpp>

#include <algorithm>

namespace hog::client {
namespace {

// Whether to go down into `child` of a node wanted deeper than a region: not where it lies in a
// closed leaf, nor in an open one the surface misses but where the finest is wanted. A leaf at a
// piece's foot is open for what's under it, so it's gone into wherever the surface may be.
bool worth_going_into(const HeldPieces& held, const grid::View& view, grid::Key child, DVec3 eye) {
    const grid::Grid& grid = held.grid();
    const auto known = held.at<grid::Shape>(child.child(0));
    if (!known) {
        return false;
    }
    const auto& found = known->found;
    if (found.depth > child.depth) {
        return true;
    }
    if (!found.open) {
        return false;
    }
    const bool foot = held.reach(known->root) == found.depth;
    const bool missed = found.value.is_solid() || (!foot && !found.value.crosses());
    return !missed || grid.distance(child, eye) <= grid.finest() / view.angle;
}

// Nearest first, however big: what's far waits on what's near, so the ground about the player is
// filled and sent before the planet's coarse levels round it.
double priority_of(const grid::Grid& grid, grid::Key node, double away, DVec3 eye,
                   std::optional<DVec3> facing) {
    double priority = 1.0 / (away + grid.finest());
    // Wholly behind where the eye looks: its nearest point is behind the eye.
    if (facing && away > 0.0) {
        const DVec3 low = grid.low(node);
        const DVec3 nearest = glm::clamp(eye, low, low + DVec3{grid.size(node.depth)});
        if (glm::dot(nearest - eye, *facing) < 0.0) {
            priority *= behind_eye;
        }
    }
    return priority;
}

} // namespace

std::uint8_t depth_wanted(const grid::Grid& grid, const grid::View& view, grid::Key node,
                          double metres) noexcept {
    for (std::uint8_t depth = node.depth; depth < grid.depth(); ++depth) {
        if (!view.coarse(grid, depth, metres)) {
            return depth;
        }
    }
    return grid.depth();
}

std::vector<PieceWish> pieces_to_ask(const HeldPieces& held, DVec3 eye, std::optional<DVec3> facing,
                                     const grid::View& view, std::size_t most) {
    const grid::Grid& grid = held.grid();
    std::vector<PieceWish> wishes;
    const auto wish = [&](grid::Key node, std::uint8_t depth, double away) {
        if (!held.covers(node, depth)) {
            wishes.push_back({.node = node,
                              .depth = depth,
                              .priority = priority_of(grid, node, away, eye, facing)});
        }
    };
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key node = pending.back();
        pending.pop_back();
        const double away = grid.distance(node, eye);
        const std::uint8_t depth = depth_wanted(grid, view, node, away);
        if (depth == node.depth) {
            continue;
        }
        if (depth - node.depth <= region_levels) {
            wish(node, depth, away);
            continue;
        }
        if (!held.covers(node, static_cast<std::uint8_t>(node.depth + 1))) {
            wish(node, static_cast<std::uint8_t>(node.depth + region_levels), away);
            continue;
        }
        for (std::size_t octant = 0; octant < 8; ++octant) {
            if (worth_going_into(held, view, node.child(octant), eye)) {
                pending.push_back(node.child(octant));
            }
        }
    }
    std::ranges::sort(wishes, [](const PieceWish& a, const PieceWish& b) {
        return a.priority != b.priority ? a.priority > b.priority : grid::ZOrder{}(a.node, b.node);
    });
    if (wishes.size() > most) {
        wishes.resize(most);
    }
    return wishes;
}

protocol::AskPieces ask_of(const std::vector<PieceWish>& wishes) {
    protocol::AskPieces ask;
    ask.pieces.reserve(std::min(wishes.size(), protocol::most_asked));
    for (const PieceWish& wish : wishes) {
        if (ask.pieces.size() == protocol::most_asked) {
            break;
        }
        ask.pieces.push_back({.depth = wish.node.depth,
                              .x = wish.node.x,
                              .y = wish.node.y,
                              .z = wish.node.z,
                              .to = wish.depth,
                              .priority = static_cast<float>(wish.priority)});
    }
    return ask;
}

} // namespace hog::client
