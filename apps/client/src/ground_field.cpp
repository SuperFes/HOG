#include <hog/client/ground_field.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace hog::client {
namespace {

// Along which axis the ends of an edge lie apart.
glm::length_t axis_of(I64Vec3 low, I64Vec3 high) noexcept {
    if (high.x != low.x) {
        return 0;
    }
    return high.y != low.y ? 1 : 2;
}

} // namespace

GroundField::GroundField(const PagesSnapshot& pages)
    : pages_{&pages}, grid_{pages.grid()}, origin_{grid_.low(grid::Key{})},
      across_{std::int64_t{1} << grid_.depth()} {}

DVec3 GroundField::at(I64Vec3 point) const noexcept {
    return origin_ + (DVec3{point} * grid_.finest());
}

double GroundField::distance(const Leaf& leaf, DVec3 point) noexcept {
    return leaf.out + glm::dot(leaf.normal, point - leaf.centre);
}

const GroundField::Leaf& GroundField::leaf(I64Vec3 point) {
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
        const auto known = pages_->at<grid::Shape>(node);
        made_leaf.node = known ? node.ancestor(known->found.depth) : grid::Key{};
        made_leaf.value = known ? known->found.value : grid::Surface::open();
        const auto shift = static_cast<unsigned>(grid_.depth() - made_leaf.node.depth);
        made_leaf.low = I64Vec3{made_leaf.node.x, made_leaf.node.y, made_leaf.node.z} *
                        (std::int64_t{1} << shift);
        made_leaf.high = made_leaf.low + I64Vec3{std::int64_t{1} << shift};
        if (made_leaf.value.crosses()) {
            made_leaf.normal = made_leaf.value.normal();
            made_leaf.centre = grid_.centre(made_leaf.node);
            made_leaf.out = made_leaf.value.distance() * grid_.size(made_leaf.node.depth);
        }
    }
    recent_[next_recent_] = &found->second;
    next_recent_ = (next_recent_ + 1) % recent_.size();
    return found->second;
}

bool GroundField::holds(const Leaf& leaf, I64Vec3 point, bool inside) noexcept {
    return inside ? glm::all(glm::lessThan(leaf.low, point)) &&
                        glm::all(glm::lessThan(point, leaf.high))
                  : glm::all(glm::lessThanEqual(leaf.low, point)) &&
                        glm::all(glm::lessThanEqual(point, leaf.high));
}

GroundField::Sample GroundField::sample(I64Vec3 point) {
    std::array<grid::Key, 8> crossing{};
    std::size_t crossings = 0;
    int solid = 0;
    int open = 0;
    double smallest = std::numeric_limits<double>::max();
    double nearest = std::numeric_limits<double>::max();
    Sample sampled;
    for (unsigned octant = 0; octant < 8; ++octant) {
        const I64Vec3 node =
            point - I64Vec3{(octant & 1U) == 0U ? 1 : 0, (octant & 2U) == 0U ? 1 : 0,
                            (octant & 4U) == 0U ? 1 : 0};
        if (glm::any(glm::lessThan(node, I64Vec3{0})) ||
            glm::any(glm::greaterThanEqual(node, I64Vec3{across_}))) {
            continue;
        }
        const Leaf& found = leaf(node);
        if (!found.value.crosses()) {
            (found.value.is_solid() ? solid : open) += 1;
            smallest = glm::min(smallest, grid_.size(found.node.depth));
        } else if (const auto seen = std::span{crossing}.first(crossings);
                   std::ranges::find(seen, found.node) == seen.end()) {
            crossing[crossings++] = found.node;
            const double away = distance(found, at(point));
            sampled.value += away;
            sampled.normal += found.normal;
            if (std::abs(away) < nearest) {
                nearest = std::abs(away);
                sampled.material = found.value.material();
            }
        }
        // Inside one leaf, every node round the point lies in it.
        if (holds(found, point, true)) {
            break;
        }
    }
    if (crossings > 0) {
        sampled.value /= static_cast<double>(crossings);
        sampled.normal = glm::normalize(sampled.normal);
    } else {
        sampled.value = (solid > open ? -0.5 : 0.5) * smallest;
    }
    return sampled;
}

double GroundField::value(I64Vec3 point) {
    return sample(point).value;
}

const GroundField::Leaf* GroundField::holding(I64Vec3 low, I64Vec3 high) {
    const glm::length_t axis = axis_of(low, high);
    const glm::length_t one = (axis + 1) % 3;
    const glm::length_t two = (axis + 2) % 3;
    const Leaf* found = nullptr;
    for (const std::int64_t first : {-1, 0}) {
        for (const std::int64_t second : {-1, 0}) {
            I64Vec3 node = low;
            node[one] += first;
            node[two] += second;
            if (node[one] < 0 || node[one] >= across_ || node[two] < 0 || node[two] >= across_) {
                return nullptr;
            }
            const Leaf& next = leaf(node);
            if (found != nullptr && found->node != next.node) {
                return nullptr;
            }
            found = &next;
        }
    }
    return found != nullptr && holds(*found, high, false) ? found : nullptr;
}

Crossed GroundField::crossed(DVec3 point, const Sample& near, I64Vec3 low, I64Vec3 high,
                             bool low_solid) {
    DVec3 normal = near.normal;
    if (glm::dot(normal, normal) == 0.0) {
        // Out of the solid along the edge.
        normal = glm::normalize(DVec3{high - low}) * (low_solid ? 1.0 : -1.0);
    }
    return {.at = point,
            .normal = Vec3{normal},
            .materials = {near.material, near.material},
            .share = 1.0F};
}

Crossed GroundField::crossing(I64Vec3 low, I64Vec3 high) {
    I64Vec3 from = low;
    I64Vec3 to = high;
    Sample at_from = sample(from);
    Sample at_to = sample(to);
    const bool low_solid = at_from.value < 0.0;
    while (true) {
        if (const Leaf* whole = holding(from, to)) {
            if (!whole->value.crosses()) {
                // Within a leaf with no plane, the ground lies at the end it disagrees with.
                const bool at_low = whole->value.is_solid() != low_solid;
                const I64Vec3 end = at_low ? from : to;
                return crossed(at(end), at_low ? at_from : at_to, low, high, low_solid);
            }
            const double a = distance(*whole, at(from));
            const double b = distance(*whole, at(to));
            if ((a < 0.0) != (b < 0.0)) {
                const DVec3 point = glm::mix(at(from), at(to), a / (a - b));
                return {.at = point,
                        .normal = Vec3{whole->normal},
                        .materials = {whole->value.material(), whole->value.material()},
                        .share = 1.0F};
            }
        }
        const I64Vec3 half = (to - from) / std::int64_t{2};
        if (half == I64Vec3{0}) {
            break;
        }
        const I64Vec3 middle = from + half;
        const Sample at_middle = sample(middle);
        if ((at_middle.value < 0.0) == low_solid) {
            from = middle;
            at_from = at_middle;
        } else {
            to = middle;
            at_to = at_middle;
        }
    }
    const double apart = at_from.value - at_to.value;
    const double along = apart == 0.0 ? 0.5 : glm::clamp(at_from.value / apart, 0.0, 1.0);
    const Sample& near = std::abs(at_from.value) <= std::abs(at_to.value) ? at_from : at_to;
    return crossed(glm::mix(at(from), at(to), along), near, low, high, low_solid);
}

BlockField GroundField::field() {
    return {.value = [this](I64Vec3 point) { return value(point); },
            .crossing = [this](I64Vec3 low, I64Vec3 high) { return crossing(low, high); }};
}

} // namespace hog::client
