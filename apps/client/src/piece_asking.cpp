#include <hog/client/piece_asking.hpp>

#include <algorithm>
#include <set>

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

namespace {

// The deepest depth short of `to` that `held` covers all of `node` down to; 0 where it doesn't
// cover the node itself.
std::uint8_t held_down_to(const HeldPieces& held, grid::Key node, std::uint8_t to) {
    if (!held.covers(node, node.depth)) {
        return 0;
    }
    std::uint8_t depth = node.depth;
    while (depth + 1 < to && held.covers(node, static_cast<std::uint8_t>(depth + 1))) {
        ++depth;
    }
    return depth;
}

} // namespace

std::optional<protocol::AskPieces> PieceRequests::ask(const HeldPieces& held,
                                                      std::span<const PieceWish> wishes) {
    protocol::AskPieces ask;
    Needed needed;
    const auto add = [&](const PieceWish& wish) {
        ask.pieces.push_back({.depth = wish.node.depth,
                              .x = wish.node.x,
                              .y = wish.node.y,
                              .z = wish.node.z,
                              .to = wish.depth,
                              .held = held_down_to(held, wish.node, wish.depth),
                              .priority = static_cast<float>(wish.priority)});
    };
    for (const PieceWish& wish : wishes) {
        if (const auto found = open_.find(wish.node); found != open_.end()) {
            needed.insert(wish.node);
            if (found->second.depth < wish.depth && ask.pieces.size() < protocol::most_asked) {
                // Asked again deeper, in place of what was asked.
                asked_ += found->second.waiting ? 1U : 0U;
                found->second = {.depth = wish.depth, .waiting = false, .priority = wish.priority};
                add(wish);
            }
            continue;
        }
        // Coming whole with a node over it that's open deep enough.
        if (const std::optional<grid::Key> over = open_over(wish.node, wish.depth)) {
            needed.insert(*over);
            continue;
        }
        if (asked_ >= window_ || ask.pieces.size() >= protocol::most_asked) {
            continue;
        }
        open_.emplace(wish.node, Open{.depth = wish.depth, .priority = wish.priority});
        ++asked_;
        needed.insert(wish.node);
        add(wish);
    }
    drop_unneeded(needed, ask);
    if (ask.pieces.empty() && ask.dropped.empty()) {
        return std::nullopt;
    }
    return ask;
}

std::optional<grid::Key> PieceRequests::open_over(grid::Key node, std::uint8_t depth) const {
    for (std::uint8_t over = node.depth; over-- > 0;) {
        const grid::Key key = node.ancestor(over);
        if (const auto found = open_.find(key);
            found != open_.end() && found->second.depth >= depth) {
            return key;
        }
    }
    return std::nullopt;
}

void PieceRequests::drop_unneeded(const Needed& needed, protocol::AskPieces& ask) {
    for (auto at = open_.begin(); at != open_.end() && ask.dropped.size() < protocol::most_asked;) {
        if (needed.contains(at->first)) {
            ++at;
            continue;
        }
        ask.dropped.push_back(
            {.depth = at->first.depth, .x = at->first.x, .y = at->first.y, .z = at->first.z});
        asked_ -= at->second.waiting ? 0U : 1U;
        at = open_.erase(at);
    }
}

std::vector<PieceWish> PieceRequests::open() const {
    std::vector<PieceWish> wishes;
    wishes.reserve(open_.size());
    for (const auto& [node, open] : open_) {
        wishes.push_back({.node = node, .depth = open.depth, .priority = open.priority});
    }
    return wishes;
}

void PieceRequests::done(std::span<const protocol::DonePiece> done) {
    for (const protocol::DonePiece& piece : done) {
        const grid::Key node{.depth = piece.depth, .x = piece.x, .y = piece.y, .z = piece.z};
        const auto found = open_.find(node);
        // Told of what was asked before it was asked again deeper, more is still to come.
        if (found == open_.end() || found->second.depth != piece.to) {
            continue;
        }
        if (!found->second.waiting) {
            --asked_;
        }
        if (piece.whole) {
            open_.erase(found);
        } else {
            found->second.waiting = true;
        }
    }
}

} // namespace hog::client
