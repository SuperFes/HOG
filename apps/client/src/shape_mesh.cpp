#include <hog/client/shape_mesh.hpp>
#include <hog/core/assert.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <unordered_map>

namespace hog::client {

namespace {

// A node of the page as drawn: its depth under the page's root and place among that depth's.
struct Node {
    std::uint8_t depth = 0;
    std::array<std::uint32_t, 3> at{};
    bool outside = false; // past the page's sides

    [[nodiscard]] std::uint64_t code() const noexcept {
        return grid::code(grid::Key{.depth = depth, .x = at[0], .y = at[1], .z = at[2]});
    }
};

// The axes across an edge along each axis, so the three turn the right-handed way.
constexpr std::array<std::size_t, 3> across_u{1, 2, 0};
constexpr std::array<std::size_t, 3> across_v{2, 0, 1};

// Of eight children, the one on side `along` of `axis` and sides `u`, `v` of the axes across it.
std::uint32_t octant(std::size_t axis, std::uint32_t along, std::uint32_t u,
                     std::uint32_t v) noexcept {
    return (along << axis) | (u << across_u.at(axis)) | (v << across_v.at(axis));
}

// Up to four vertices round an edge, in the order they turn.
struct Polygon {
    std::array<std::uint32_t, 4> vertices{};
    std::array<std::uint8_t, 4> depths{}; // of the leaves they're in
    std::size_t count = 0;
};

// Leaves are drawn at most this many levels over the drawn depth, so a page that's one wide leaf
// still draws its surface, and triangles don't run long.
constexpr int widest_drawn = 2;

// Places are in the drawn depth's nodes from the page's low corner.
class Mesher {
public:
    using Found = grid::Tree<grid::Shape>::Found;

    Mesher(const grid::Tree<grid::Shape>& tree, std::uint8_t levels)
        : tree_{&tree}, levels_{levels} {}

    // The page, and nothing past its sides: edges along its sides are met too, for where the
    // leaves beside them are crossed, but make no polygons.
    void page() {
        const Node root{};
        const Node none{.outside = true};
        cell(root);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            face(none, root, axis);
            face(root, none, axis);
            for (std::size_t i = 0; i < 4; ++i) {
                std::array<Node, 4> around{none, none, none, none};
                around.at(i) = root;
                edge(around, axis);
            }
        }
    }

    // Cell, face and edge passes after Ju et al., "Dual Contouring of Hermite Data": each edge
    // where solid meets open is met once, as the smallest of the four leaves round it has it.
    // NOLINTNEXTLINE(misc-no-recursion): as deep as the page's levels
    void cell(const Node& node) {
        if (leaf(node)) {
            return;
        }
        std::array<Node, 8> children{};
        for (std::uint32_t k = 0; k < 8; ++k) {
            children.at(k) = child(node, k);
            cell(children.at(k));
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::uint32_t bit = 1U << static_cast<unsigned>(axis);
            for (std::uint32_t k = 0; k < 8; ++k) {
                if ((k & bit) == 0) {
                    face(children.at(k), children.at(k | bit), axis);
                }
            }
            for (std::uint32_t half = 0; half < 2; ++half) {
                std::array<Node, 4> around{};
                for (std::uint32_t i = 0; i < 4; ++i) {
                    around.at(i) = children.at(octant(axis, half, i & 1U, i >> 1U));
                }
                edge(around, axis);
            }
        }
    }

    [[nodiscard]] ShapeMesh mesh(double metres) const {
        ShapeMesh mesh;
        mesh.vertices.resize(nodes_.size());
        std::vector<DVec3> positions(nodes_.size());
        std::vector<bool> crosses(nodes_.size());
        for (std::size_t id = 0; id < nodes_.size(); ++id) {
            const Node& node = nodes_[id];
            const Found found = held(node);
            const grid::Surface& value = found.value;
            const Crossings& crossings = crossings_.at(node.code());
            const DVec3 mass = crossings.sum / static_cast<double>(crossings.count);
            crosses[id] = value.crosses();
            positions[id] = crosses[id] ? on_plane(node, found, mass) : mass;
            ShapeVertex& vertex = mesh.vertices[id];
            vertex.position = Vec3{positions[id] * metres};
            if (crosses[id]) {
                vertex.normal = Vec3{value.normal()};
                const grid::Blend blend = grid::Blend::of(value.material());
                vertex.materials = {blend.first, blend.second};
                vertex.share = static_cast<float>(blend.share);
            }
        }
        for (const Polygon& polygon : polygons_) {
            const auto& v = polygon.vertices;
            if (polygon.count == 3) {
                mesh.indices.insert(mesh.indices.end(), {v[0], v[1], v[2]});
            } else if (split_first(polygon, positions)) {
                mesh.indices.insert(mesh.indices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
            } else {
                mesh.indices.insert(mesh.indices.end(), {v[0], v[1], v[3], v[1], v[2], v[3]});
            }
        }
        // A leaf the surface meets only along its sides faces as its triangles do, and shows what
        // the leaves it's joined to show.
        std::vector<DVec3> facing(nodes_.size(), DVec3{0.0});
        std::vector<grid::Blending> showing(nodes_.size());
        for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
            const std::array<std::uint32_t, 3> corner{mesh.indices[t], mesh.indices[t + 1],
                                                      mesh.indices[t + 2]};
            const DVec3 area = glm::cross(positions[corner[1]] - positions[corner[0]],
                                          positions[corner[2]] - positions[corner[0]]);
            for (const std::uint32_t id : corner) {
                facing[id] += area;
                for (const std::uint32_t other : corner) {
                    if (crosses[other]) {
                        showing[id].add(mesh.vertices[other].materials[0],
                                        static_cast<double>(mesh.vertices[other].share));
                        showing[id].add(mesh.vertices[other].materials[1],
                                        1.0 - static_cast<double>(mesh.vertices[other].share));
                    }
                }
            }
        }
        for (std::size_t id = 0; id < nodes_.size(); ++id) {
            if (crosses[id]) {
                continue;
            }
            ShapeVertex& vertex = mesh.vertices[id];
            if (glm::dot(facing[id], facing[id]) > 0.0) {
                vertex.normal = Vec3{glm::normalize(facing[id])};
            }
            const grid::Blend blend = grid::Blend::of(showing[id].entry());
            vertex.materials = {blend.first, blend.second};
            vertex.share = static_cast<float>(blend.share);
        }
        return mesh;
    }

private:
    struct Crossings {
        DVec3 sum{0.0};
        int count = 0;
    };

    // Whether a quad is split from its first vertex to its third. Two wide leaves can share
    // several short edges, each with a quad they're opposite in, so quads are split through their
    // finer leaves, else across the shorter way.
    [[nodiscard]] static bool split_first(const Polygon& polygon,
                                          const std::vector<DVec3>& positions) noexcept {
        const auto& v = polygon.vertices;
        const auto& d = polygon.depths;
        if (d[0] + d[2] != d[1] + d[3]) {
            return d[0] + d[2] > d[1] + d[3];
        }
        return glm::length(positions[v[0]] - positions[v[2]]) <=
               glm::length(positions[v[1]] - positions[v[3]]);
    }

    [[nodiscard]] std::uint32_t span(const Node& node) const noexcept {
        return 1U << static_cast<unsigned>(levels_ - node.depth);
    }

    [[nodiscard]] std::uint8_t widest() const noexcept {
        return static_cast<std::uint8_t>(std::max(levels_ - widest_drawn, 0));
    }

    // A leaf of the tree drawn wider than `widest_drawn` allows is drawn as its children.
    [[nodiscard]] bool leaf(const Node& node) const noexcept {
        return node.outside || node.depth == levels_ ||
               (node.depth >= widest() &&
                tree_->at(node.depth + 1, 2 * node.at[0], 2 * node.at[1], 2 * node.at[2]).depth <=
                    node.depth);
    }

    // The tree's leaf at or over `node`.
    [[nodiscard]] Found held(const Node& node) const noexcept {
        return tree_->at(node.depth, node.at[0], node.at[1], node.at[2]);
    }

    // A leaf stands for its own children.
    [[nodiscard]] Node child(const Node& node, std::uint32_t k) const noexcept {
        if (leaf(node)) {
            return node;
        }
        return {.depth = static_cast<std::uint8_t>(node.depth + 1),
                .at = {(2 * node.at[0]) + (k & 1U), (2 * node.at[1]) + ((k >> 1U) & 1U),
                       (2 * node.at[2]) + ((k >> 2U) & 1U)}};
    }

    // NOLINTNEXTLINE(misc-no-recursion): as deep as the page's levels
    void face(const Node& low, const Node& high, std::size_t axis) {
        if (leaf(low) && leaf(high)) {
            return;
        }
        for (std::uint32_t i = 0; i < 4; ++i) {
            face(child(low, octant(axis, 1, i & 1U, i >> 1U)),
                 child(high, octant(axis, 0, i & 1U, i >> 1U)), axis);
        }
        for (const std::size_t along : {across_u.at(axis), across_v.at(axis)}) {
            for (std::uint32_t half = 0; half < 2; ++half) {
                edge(in_face(low, high, axis, along, half), along);
            }
        }
    }

    // Round the edge along `along` in the face across `axis` between `low` and `high`, on side
    // `half` of the face's middle.
    [[nodiscard]] std::array<Node, 4> in_face(const Node& low, const Node& high, std::size_t axis,
                                              std::size_t along, std::uint32_t half) const {
        const std::size_t other = 3 - axis - along;
        std::array<Node, 4> around{};
        for (std::uint32_t side = 0; side < 2; ++side) {
            for (std::uint32_t beside = 0; beside < 2; ++beside) {
                const std::uint32_t k = ((1U - side) << static_cast<unsigned>(axis)) |
                                        (beside << static_cast<unsigned>(other)) |
                                        (half << static_cast<unsigned>(along));
                const std::uint32_t u = across_u.at(along) == axis ? side : beside;
                const std::uint32_t v = across_v.at(along) == axis ? side : beside;
                around.at(u + (2 * v)) = child(side == 0 ? low : high, k);
            }
        }
        return around;
    }

    // Four nodes round an edge along `axis`, by their sides of it on the axes across.
    // NOLINTNEXTLINE(misc-no-recursion): as deep as the page's levels
    void edge(const std::array<Node, 4>& around, std::size_t axis) {
        if (std::ranges::all_of(around, [&](const Node& node) { return leaf(node); })) {
            crossing(around, axis);
            return;
        }
        for (std::uint32_t half = 0; half < 2; ++half) {
            std::array<Node, 4> finer{};
            for (std::uint32_t i = 0; i < 4; ++i) {
                finer.at(i) =
                    child(around.at(i), octant(axis, half, 1U - (i & 1U), 1U - (i >> 1U)));
            }
            edge(finer, axis);
        }
    }

    void crossing(const std::array<Node, 4>& around, std::size_t axis) {
        std::size_t smallest = 4;
        bool outside = false;
        for (std::size_t i = 0; i < 4; ++i) {
            outside = outside || around.at(i).outside;
            if (!around.at(i).outside &&
                (smallest == 4 || around.at(i).depth > around.at(smallest).depth)) {
                smallest = i;
            }
        }
        const Node& owner = around.at(smallest);
        const std::uint32_t side = span(owner);
        std::array<std::uint32_t, 3> from{owner.at[0] * side, owner.at[1] * side,
                                          owner.at[2] * side};
        const auto quarter = static_cast<std::uint32_t>(smallest);
        from.at(across_u.at(axis)) += (1U - (quarter & 1U)) * side;
        from.at(across_v.at(axis)) += (1U - (quarter >> 1U)) * side;
        std::array<std::uint32_t, 3> to = from;
        to.at(axis) += side;
        const double first = value(from);
        const double last = value(to);
        if ((first < 0.0) == (last < 0.0)) {
            return;
        }
        DVec3 point{from[0], from[1], from[2]};
        point[static_cast<glm::length_t>(axis)] += side * first / (first - last);
        for (std::size_t i = 0; i < 4; ++i) {
            const Node& node = around.at(i);
            const std::span<const Node> before = std::span{around}.first(i);
            if (!node.outside &&
                std::ranges::find(before, node.code(), &Node::code) == before.end()) {
                Crossings& crossings = crossings_[node.code()];
                crossings.sum += point;
                ++crossings.count;
            }
        }
        if (outside) {
            return;
        }

        // Solid at the low end faces the edge's way, and round it the order turns anticlockwise.
        constexpr std::array<std::size_t, 4> turning{0, 1, 3, 2};
        Polygon polygon;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t at = turning.at(first < 0.0 ? i : 3 - i);
            const std::uint32_t id = vertex(around.at(at));
            if (polygon.count == 0 ||
                (polygon.vertices.at(polygon.count - 1) != id && polygon.vertices[0] != id)) {
                polygon.depths.at(polygon.count) = around.at(at).depth;
                polygon.vertices.at(polygon.count++) = id;
            }
        }
        if (polygon.count < 3) {
            return;
        }
        polygons_.push_back(polygon);
    }

    std::uint32_t vertex(const Node& node) {
        const auto [found, added] =
            ids_.try_emplace(node.code(), static_cast<std::uint32_t>(nodes_.size()));
        if (added) {
            nodes_.push_back(node);
        }
        return found->second;
    }

    // How far out of the solid a corner lies, in nodes: the mean of where the planes of the
    // smallest leaves touching it put it, but solid where it touches solid leaves and no open ones,
    // and open the other way round, so leaves all solid or open have no surface through them.
    double value(const std::array<std::uint32_t, 3>& corner) {
        const std::uint64_t code =
            corner[0] | (std::uint64_t{corner[1]} << 21U) | (std::uint64_t{corner[2]} << 42U);
        if (const auto known = values_.find(code); known != values_.end()) {
            return known->second;
        }
        const auto per_side = std::int64_t{1} << levels_;
        std::array<std::uint64_t, 8> seen{};
        std::size_t seen_count = 0;
        int deepest = -1;
        double sum = 0.0;
        int count = 0;
        int solid = 0;
        int open = 0;
        for (std::uint32_t k = 0; k < 8; ++k) {
            std::array<std::int64_t, 3> at{};
            bool inside = true;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                at.at(axis) = std::int64_t{corner.at(axis)} - 1 + ((k >> axis) & 1U);
                inside = inside && at.at(axis) >= 0 && at.at(axis) < per_side;
            }
            if (!inside) {
                continue;
            }
            const auto found =
                tree_->at(levels_, static_cast<std::uint32_t>(at[0]),
                          static_cast<std::uint32_t>(at[1]), static_cast<std::uint32_t>(at[2]));
            const std::uint8_t depth = std::max(found.depth, widest());
            const auto up = static_cast<unsigned>(levels_ - depth);
            const Node node{.depth = depth,
                            .at = {static_cast<std::uint32_t>(at[0]) >> up,
                                   static_cast<std::uint32_t>(at[1]) >> up,
                                   static_cast<std::uint32_t>(at[2]) >> up}};
            const std::uint64_t node_code = node.code();
            const std::span<const std::uint64_t> earlier = std::span{seen}.first(seen_count);
            if (std::ranges::find(earlier, node_code) != earlier.end()) {
                continue;
            }
            seen.at(seen_count++) = node_code;
            solid += found.value.is_solid() ? 1 : 0;
            open += found.value.is_open() ? 1 : 0;
            if (!found.value.crosses()) {
                continue;
            }
            if (int{depth} > deepest) {
                deepest = depth;
                sum = 0.0;
                count = 0;
            }
            if (int{depth} == deepest) {
                sum += past_plane(node, found, DVec3{corner[0], corner[1], corner[2]});
                ++count;
            }
        }
        // Off the corner, so the surface crosses an edge short of it.
        constexpr double least = 1.0 / 64.0;
        double out = count > 0 ? sum / count : least * (open - solid);
        if (solid > 0 && open == 0) {
            out = std::min(out, -least);
        } else if (open > 0 && solid == 0) {
            out = std::max(out, least);
        }
        values_.emplace(code, out);
        return out;
    }

    [[nodiscard]] DVec3 middle(const Node& node) const noexcept {
        return (DVec3{node.at[0], node.at[1], node.at[2]} + 0.5) * static_cast<double>(span(node));
    }

    // How far out of the solid `point` lies by the plane of `found`, the tree's leaf over `node`.
    [[nodiscard]] double past_plane(const Node& node, const Found& found,
                                    DVec3 point) const noexcept {
        const auto up = static_cast<unsigned>(node.depth - found.depth);
        const Node over{.depth = found.depth,
                        .at = {node.at[0] >> up, node.at[1] >> up, node.at[2] >> up}};
        return (found.value.distance() * span(over)) +
               glm::dot(found.value.normal(), point - middle(over));
    }

    // Where `point` meets the plane of `found`, the tree's leaf over `node`, straight along it,
    // kept in the node.
    [[nodiscard]] DVec3 on_plane(const Node& node, const Found& found, DVec3 point) const noexcept {
        point -= found.value.normal() * past_plane(node, found, point);
        const auto side = static_cast<double>(span(node));
        const DVec3 low = DVec3{node.at[0], node.at[1], node.at[2]} * side;
        return glm::clamp(point, low, low + side);
    }

    const grid::Tree<grid::Shape>* tree_;
    std::uint8_t levels_;
    std::unordered_map<std::uint64_t, double> values_;
    std::unordered_map<std::uint64_t, std::uint32_t> ids_;
    std::vector<Node> nodes_;
    std::unordered_map<std::uint64_t, Crossings> crossings_; // by the leaves round them
    std::vector<Polygon> polygons_;
};

} // namespace

ShapeMesh mesh_shape(const grid::Grid& grid, grid::Key root, const grid::Tree<grid::Shape>& tree,
                     std::uint8_t levels) {
    HOG_ASSERT(levels <= tree.levels(), "drawn no deeper than the tree");
    Mesher mesher{tree, levels};
    mesher.page();
    ShapeMesh mesh = mesher.mesh(grid.size(static_cast<std::uint8_t>(root.depth + levels)));
    mesh.origin = grid.low(root);
    return mesh;
}

} // namespace hog::client
