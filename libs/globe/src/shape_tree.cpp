#include <hog/core/assert.hpp>
#include <hog/globe/shape_tree.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <optional>
#include <utility>
#include <variant>

namespace hog::globe {
namespace {

// Lays `primitive` over what `inside` held at `point`.
void lay(Inside& inside, const Primitive& primitive, DVec3 point) noexcept {
    const double away = distance(primitive.form, point);
    if (!primitive.carves) {
        if (away < inside.distance) {
            inside.distance = away;
            if (away < 0.0) {
                inside.material = primitive.material;
            }
        }
    } else if (-away > inside.distance) {
        inside.distance = -away;
        if (inside.distance >= 0.0) {
            inside.material = Material::Air;
        }
    }
}

} // namespace

double distance(const Form& form, DVec3 point) noexcept {
    if (const auto* ball = std::get_if<Ball>(&form)) {
        return glm::length(point - ball->middle) - ball->radius;
    }
    if (const auto* half = std::get_if<HalfSpace>(&form)) {
        return glm::dot(half->normal, point) - half->offset;
    }
    const Box& box = *std::get_if<Box>(&form);
    const DVec3 off = point - box.middle;
    const DVec3 out = glm::abs(DVec3{glm::dot(off, box.axes[0]), glm::dot(off, box.axes[1]),
                                     glm::dot(off, box.axes[2])}) -
                      box.half;
    return glm::length(glm::max(out, DVec3{0.0})) + std::min(std::max({out.x, out.y, out.z}), 0.0);
}

double Summary::solid() const noexcept {
    return std::accumulate(of.begin(), of.end(), 0.0);
}

Summary combine(std::span<const Summary> parts) noexcept {
    Summary whole;
    whole.distance = {.low = std::numeric_limits<double>::infinity(),
                      .high = -std::numeric_limits<double>::infinity()};
    for (const Summary& part : parts) {
        whole.volume += part.volume;
        for (std::size_t m = 0; m < whole.of.size(); ++m) {
            whole.of[m] += part.of[m];
        }
        whole.distance.low = std::min(whole.distance.low, part.distance.low);
        whole.distance.high = std::max(whole.distance.high, part.distance.high);
        whole.estimated = whole.estimated || part.estimated;
    }
    return whole;
}

ShapeTree::ShapeTree(std::vector<Primitive> primitives, double inner, double outer, Limits limits)
    : primitives_{std::move(primitives)}, inner_{inner}, outer_{outer}, limits_{limits} {
    HOG_ASSERT(inner < outer && outer - inner <= width_at(0), "a shell thinner than a face");
    std::vector<std::uint32_t> all(primitives_.size());
    for (std::size_t index = 0; index < all.size(); ++index) {
        all[index] = static_cast<std::uint32_t>(index);
    }
    for (std::uint8_t face = 0; face < 6; ++face) {
        roots_[face] = make({.face = face}, all, false);
        build(roots_[face]);
    }
}

double ShapeTree::width_at(int level) const noexcept {
    return std::ldexp(std::sqrt(4.0 * std::numbers::pi / 6.0) * outer_, -level);
}

ShapeTree::Region ShapeTree::region_of(NodeKey key) const noexcept {
    const double width = width_at(key.level);
    const double low = inner_ + (static_cast<double>(key.k) * width);
    return {.cell = key.cell(), .low = low, .high = std::min(low + width, outer_)};
}

NodeKey ShapeTree::key_under(DVec3 point, int level) const {
    const Cell cell = cell_under(glm::normalize(point), level);
    const double width = width_at(level);
    const double up = std::clamp(glm::length(point), inner_, outer_) - inner_;
    const auto spans = static_cast<std::uint32_t>(std::ceil((outer_ - inner_) / width));
    return {.level = cell.level,
            .face = cell.face,
            .i = cell.i,
            .j = cell.j,
            .k = std::min(static_cast<std::uint32_t>(up / width), spans - 1)};
}

double ShapeTree::reach_of(const Region& region, DVec3 toward, double middle) noexcept {
    double widest = 1.0;
    for (const DVec3& corner : corners(region.cell)) {
        widest = std::min(widest, glm::dot(toward, corner));
    }
    double reach = 0.0;
    for (const double radius : {region.low, region.high}) {
        reach = std::max(reach, std::sqrt(std::max(0.0, (radius * radius) + (middle * middle) -
                                                            (2.0 * radius * middle * widest))));
    }
    return reach;
}

Interval ShapeTree::prune(std::span<const std::uint32_t> from, DVec3 at, double reach,
                          std::vector<std::uint32_t>& kept, bool& filled) const {
    const double start =
        filled ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    Interval laid{.low = start, .high = start};
    for (const std::uint32_t index : from) {
        const Primitive& primitive = primitives_[index];
        const double away = distance(primitive.form, at);
        Interval its{.low = away - reach, .high = away + reach};
        bool sets = false;
        if (primitive.carves) {
            its = {.low = -its.high, .high = -its.low};
            if (its.high <= laid.low) {
                continue;
            }
            // Only where it opens all of it: what's left solid is still of what's before it.
            sets = its.low > laid.high && its.low >= 0.0;
            laid = {.low = std::max(laid.low, its.low), .high = std::max(laid.high, its.high)};
        } else {
            if (its.low > laid.high) {
                continue;
            }
            sets = its.high < laid.low;
            laid = {.low = std::min(laid.low, its.low), .high = std::min(laid.high, its.high)};
        }
        if (sets) {
            kept.clear();
            filled = primitive.carves;
        }
        kept.push_back(index);
    }
    return laid;
}

std::optional<Material> ShapeTree::only_material(std::span<const std::uint32_t> kept) const {
    std::optional<Material> found;
    for (const std::uint32_t index : kept) {
        const Primitive& primitive = primitives_[index];
        if (primitive.carves) {
            continue;
        }
        if (found && *found != primitive.material) {
            return std::nullopt;
        }
        found = primitive.material;
    }
    return found;
}

Material ShapeTree::nearest_solid(std::span<const std::uint32_t> kept, DVec3 at) const {
    Material of = Material::Air;
    double nearest = std::numeric_limits<double>::infinity();
    for (const std::uint32_t index : kept) {
        const Primitive& primitive = primitives_[index];
        if (const double away = distance(primitive.form, at); !primitive.carves && away < nearest) {
            nearest = away;
            of = primitive.material;
        }
    }
    return of;
}

ShapeTree::Node ShapeTree::make(NodeKey key, std::span<const std::uint32_t> from,
                                bool filled) const {
    const Region region = region_of(key);
    const DVec3 toward = center(region.cell);
    const double middle = 0.5 * (region.low + region.high);
    const DVec3 at = toward * middle;
    const double reach = reach_of(region, toward, middle);
    Node node;
    node.key = key;
    node.filled = filled;
    Summary& summary = node.summary;
    summary.volume = prism_volume(region.cell, region.low, region.high);
    summary.distance = prune(from, at, reach, node.primitives, node.filled);
    if (summary.all_open()) {
        return node;
    }
    if (const std::optional<Material> one = only_material(node.primitives);
        one && summary.all_solid()) {
        summary.of.at(static_cast<std::size_t>(*one)) = summary.volume;
        return node;
    }
    // Guessed from the middle: solid by how far in it lies against how far the node reaches, of
    // what's there or the nearest solid.
    const Inside here = read(node.primitives, node.filled, at);
    const Material of =
        here.material == Material::Air ? nearest_solid(node.primitives, at) : here.material;
    const double share =
        summary.all_solid() ? 1.0 : std::clamp(0.5 - (here.distance / (2.0 * reach)), 0.0, 1.0);
    summary.of.at(static_cast<std::size_t>(of)) = share * summary.volume;
    summary.estimated = true;
    return node;
}

bool ShapeTree::whole(const Node& node) const noexcept {
    return !node.summary.estimated || node.primitives.size() <= limits_.primitives ||
           width_at(node.key.level) <= limits_.smallest || node.key.level >= Cell::max_level;
}

void ShapeTree::split(Node& node) const {
    const auto level = static_cast<std::uint8_t>(node.key.level + 1);
    const double width = width_at(level);
    node.children.reserve(8);
    for (const Cell cell : children(node.key.cell())) {
        for (std::uint32_t k = node.key.k * 2; k <= (node.key.k * 2) + 1; ++k) {
            if (inner_ + (static_cast<double>(k) * width) < outer_) {
                node.children.push_back(
                    make({.level = level, .face = cell.face, .i = cell.i, .j = cell.j, .k = k},
                         node.primitives, node.filled));
            }
        }
    }
}

void ShapeTree::build(Node& root) {
    // Depth first, each node summed from its children once they're built.
    std::vector<std::pair<Node*, bool>> waiting{{&root, false}};
    while (!waiting.empty()) {
        auto [node, built] = waiting.back();
        waiting.pop_back();
        if (built) {
            std::vector<Summary> parts;
            parts.reserve(node->children.size());
            for (const Node& child : node->children) {
                parts.push_back(child.summary);
            }
            const Interval own = node->summary.distance;
            node->summary = combine(parts);
            node->summary.distance = {.low = std::max(own.low, node->summary.distance.low),
                                      .high = std::min(own.high, node->summary.distance.high)};
            continue;
        }
        ++nodes_;
        if (whole(*node)) {
            continue;
        }
        split(*node);
        waiting.emplace_back(node, true);
        for (Node& child : node->children) {
            waiting.emplace_back(&child, false);
        }
    }
}

const ShapeTree::Node& ShapeTree::deepest(NodeKey key) const {
    const Node* node = &roots_.at(key.face);
    while (node->key.level < key.level) {
        const Node* next = nullptr;
        for (const Node& child : node->children) {
            const int shift = key.level - child.key.level;
            if (child.key.i == key.i >> shift && child.key.j == key.j >> shift &&
                child.key.k == key.k >> shift) {
                next = &child;
                break;
            }
        }
        if (next == nullptr) {
            break;
        }
        node = next;
    }
    return *node;
}

NodeKey ShapeTree::built_over(NodeKey key) const {
    return deepest(key).key;
}

const Summary& ShapeTree::summary(NodeKey key) const {
    return deepest(key).summary;
}

std::span<const std::uint32_t> ShapeTree::primitives_in(NodeKey key) const {
    return deepest(key).primitives;
}

Inside ShapeTree::read(std::span<const std::uint32_t> primitives, bool filled,
                       DVec3 point) const noexcept {
    Inside inside;
    if (filled) {
        inside.distance = -std::numeric_limits<double>::infinity();
    }
    for (const std::uint32_t index : primitives) {
        lay(inside, primitives_[index], point);
    }
    return inside;
}

Inside ShapeTree::at(DVec3 point) const noexcept {
    Inside inside;
    for (const Primitive& primitive : primitives_) {
        lay(inside, primitive, point);
    }
    return inside;
}

Inside ShapeTree::read(DVec3 point) const {
    const Node& node = deepest(key_under(point, Cell::max_level));
    return read(node.primitives, node.filled, point);
}

Inside ShapeTree::read(NodeKey key, DVec3 point) const {
    const Node& node = deepest(key);
    return read(node.primitives, node.filled, point);
}

std::vector<NodeKey> ShapeTree::leaves() const {
    std::vector<NodeKey> found;
    std::vector<const Node*> waiting;
    waiting.reserve(roots_.size());
    for (const Node& root : roots_) {
        waiting.push_back(&root);
    }
    while (!waiting.empty()) {
        const Node* node = waiting.back();
        waiting.pop_back();
        if (node->children.empty()) {
            found.push_back(node->key);
        }
        for (const Node& child : node->children) {
            waiting.push_back(&child);
        }
    }
    return found;
}

} // namespace hog::globe
