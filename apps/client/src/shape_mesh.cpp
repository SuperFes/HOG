#include <hog/client/shape_mesh.hpp>
#include <hog/core/assert.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_set>

namespace hog::client {

namespace {

// Places are in the grid's finest nodes from the root's low corner.
using Place = std::array<std::uint64_t, 3>;

struct Box {
    Place low{};
    Place high{};
};

Box box_of(grid::Key key, std::uint8_t finest) noexcept {
    const auto shift = static_cast<unsigned>(finest - key.depth);
    const std::uint64_t side = std::uint64_t{1} << shift;
    const Place low{std::uint64_t{key.x} << shift, std::uint64_t{key.y} << shift,
                    std::uint64_t{key.z} << shift};
    return {.low = low, .high = {low[0] + side, low[1] + side, low[2] + side}};
}

// Closed boxes: touching at a side, an edge or a corner counts.
bool touch(const Box& a, const Box& b) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (a.low.at(axis) > b.high.at(axis) || b.low.at(axis) > a.high.at(axis)) {
            return false;
        }
    }
    return true;
}

// Of a box within `outer`, whether it reaches outer's sides.
bool on_sides(const Box& inner, const Box& outer) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (inner.low.at(axis) == outer.low.at(axis) ||
            inner.high.at(axis) == outer.high.at(axis)) {
            return true;
        }
    }
    return false;
}

// A node as met: the place it stands for, the page drawing it, and the drawn leaf it's in.
struct Node {
    grid::Key key;
    const ShapeDrawn* page = nullptr; // none where nothing's drawn, or past the page's sides
    grid::Key leaf;
    bool is_leaf = true;
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

// The tree's leaf at or over `node`, which lies in the page.
grid::Tree<grid::Shape>::Found tree_at(const ShapeDrawn& page, grid::Key node) noexcept {
    const grid::Key root = page.page;
    const auto below = static_cast<std::uint8_t>(node.depth - root.depth);
    return page.tree->at(below, node.x - (root.x << below), node.y - (root.y << below),
                         node.z - (root.z << below));
}

// Meshes a page's own edges, or its seam's.
class Mesher {
public:
    using Found = grid::Tree<grid::Shape>::Found;

    // Its own edges where `drawn` is none.
    Mesher(const grid::Grid& grid, const ShapeDrawn& page, const ShapesDrawn* drawn)
        : page_{&page}, drawn_{drawn}, finest_{grid.depth()}, box_{box_of(page.page, finest_)},
          metres_{grid.finest()}, origin_{grid.low(page.page)},
          least_{std::ldexp(1.0 / 64.0, finest_ - page.page.depth - page.levels)} {
        HOG_ASSERT(page.levels <= page.tree->levels(), "drawn no deeper than the tree");
        // Corners are coded in 21 bits an axis.
        HOG_ASSERT(finest_ < 21, "a grid shallower than its keys allow");
    }

    void walk() {
        const Node root = make(page_->page, page_);
        cell(root);
        if (drawn_ == nullptr) {
            // Edges along its sides are met too, for where the leaves beside them are crossed.
            const Node none{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                face(none, root, axis);
                face(root, none, axis);
                for (std::size_t i = 0; i < 4; ++i) {
                    std::array<Node, 4> around{none, none, none, none};
                    around.at(i) = root;
                    edge(around, axis);
                }
            }
            return;
        }
        // Its high sides and the edges along them: those whose low leaves are its.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::size_t u = across_u.at(axis);
            const std::size_t v = across_v.at(axis);
            face(root, beside(axis == 0, axis == 1, axis == 2), axis);
            edge({root, beside(u == 0, u == 1, u == 2), beside(v == 0, v == 1, v == 2),
                  beside(u == 0 || v == 0, u == 1 || v == 1, u == 2 || v == 2)},
                 axis);
        }
    }

    [[nodiscard]] ShapeMesh mesh() {
        ShapeMesh mesh;
        mesh.origin = origin_;
        mesh.vertices.resize(nodes_.size());
        std::vector<bool> known(nodes_.size());
        for (std::size_t id = 0; id < nodes_.size(); ++id) {
            mesh.vertices[id] = vertex_of(nodes_[id]);
            known[id] = glm::dot(mesh.vertices[id].normal, mesh.vertices[id].normal) > 0.0F;
        }
        for (const Polygon& polygon : polygons_) {
            const auto& v = polygon.vertices;
            if (polygon.count == 3) {
                mesh.indices.insert(mesh.indices.end(), {v[0], v[1], v[2]});
            } else if (split_first(polygon, mesh.vertices)) {
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
            const DVec3 first{mesh.vertices[corner[0]].position};
            const DVec3 area = glm::cross(DVec3{mesh.vertices[corner[1]].position} - first,
                                          DVec3{mesh.vertices[corner[2]].position} - first);
            for (const std::uint32_t id : corner) {
                facing[id] += area;
                for (const std::uint32_t other : corner) {
                    if (known[other]) {
                        showing[id].add(mesh.vertices[other].materials[0],
                                        static_cast<double>(mesh.vertices[other].share));
                        showing[id].add(mesh.vertices[other].materials[1],
                                        1.0 - static_cast<double>(mesh.vertices[other].share));
                    }
                }
            }
        }
        for (std::size_t id = 0; id < nodes_.size(); ++id) {
            if (known[id]) {
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
        if (drawn_ == nullptr) {
            add_border(mesh);
        }
        return mesh;
    }

private:
    void add_border(ShapeMesh& mesh) {
        for (const auto& [code, crossings] : crossings_) {
            if (!near_sides(crossings.node.leaf)) {
                continue;
            }
            const auto id = ids_.find(code);
            mesh.border.emplace(code, id != ids_.end() ? mesh.vertices[id->second]
                                                       : vertex_of(crossings.node));
        }
    }

    struct Crossings {
        DVec3 sum{0.0};
        int count = 0;
        Node node;
    };
    // How far out of the solid a corner lies, in finest nodes, and whether every node round it
    // is in the page's place.
    struct Corner {
        double out = 0.0;
        bool inside = true;
    };

    // Whether a quad is split from its first vertex to its third. Two wide leaves can share
    // several short edges, each with a quad they're opposite in, so quads are split through their
    // finer leaves, else across the shorter way.
    [[nodiscard]] static bool split_first(const Polygon& polygon,
                                          const std::vector<ShapeVertex>& vertices) noexcept {
        const auto& v = polygon.vertices;
        const auto& d = polygon.depths;
        if (d[0] + d[2] != d[1] + d[3]) {
            return d[0] + d[2] > d[1] + d[3];
        }
        return glm::length(vertices[v[0]].position - vertices[v[2]].position) <=
               glm::length(vertices[v[1]].position - vertices[v[3]].position);
    }

    [[nodiscard]] std::uint64_t span(std::uint8_t depth) const noexcept {
        return std::uint64_t{1} << static_cast<unsigned>(finest_ - depth);
    }

    // Whether a page's leaf at `over` must split, holding a page drawn under it.
    [[nodiscard]] bool holds_under(const ShapeDrawn& page, grid::Key over) {
        if (page.under.empty() || over.depth >= page.page.depth + page.levels) {
            return false;
        }
        auto [found, added] = holding_.try_emplace(&page);
        if (added) {
            for (const grid::Key under : page.under) {
                for (grid::Key up = under; up.depth > page.page.depth; up = up.parent()) {
                    found->second.insert(grid::code(up.parent()));
                }
            }
        }
        return found->second.contains(grid::code(over));
    }

    // The node at `key` as `page` draws it: a leaf where it's in one, the tree's leaf or one
    // `widest_drawn` levels over the drawn depth, but split round pages drawn under it.
    [[nodiscard]] Node make(grid::Key key, const ShapeDrawn* page) {
        Node node{.key = key, .page = page, .leaf = key, .is_leaf = true};
        if (page == nullptr) {
            return node;
        }
        const grid::Key root = page->page;
        const int below = key.depth - root.depth;
        const int levels = page->levels;
        const int widest = std::max(levels - widest_drawn, 0);
        int from = 0;
        if (below >= levels) {
            from = std::max<int>(
                tree_at(*page, key.ancestor(static_cast<std::uint8_t>(root.depth + levels))).depth,
                widest);
        } else {
            const int tree_leaf = tree_at(*page, key.child(0)).depth;
            from = std::max(tree_leaf, widest);
            if (tree_leaf > below || from > below) {
                node.is_leaf = false;
                return node;
            }
        }
        const int deepest = std::min(below, levels);
        for (int depth = from; depth <= deepest; ++depth) {
            const grid::Key over = key.ancestor(static_cast<std::uint8_t>(root.depth + depth));
            if (!holds_under(*page, over)) {
                node.leaf = over;
                return node;
            }
        }
        node.is_leaf = false;
        return node;
    }

    // A leaf stands for its own children.
    [[nodiscard]] Node child(const Node& node, std::uint32_t k) {
        if (node.is_leaf) {
            return node;
        }
        const grid::Key key = node.key.child(k);
        const ShapeDrawn* page = node.page;
        if (key.depth == page->page.depth + page->levels &&
            std::ranges::binary_search(page->under, key)) {
            page = drawn_ == nullptr ? nullptr : drawn_->page(key);
        }
        return make(key, page);
    }

    // The node beside the page's root by a step along each axis, as drawn there.
    [[nodiscard]] Node beside(bool x, bool y, bool z) {
        const std::optional<grid::Key> key = page_->page.beside(x ? 1 : 0, y ? 1 : 0, z ? 1 : 0);
        return key ? make(*key, drawn_->over(*key)) : Node{};
    }

    // Whether a leaf of the page touches its sides or the pages drawn under it.
    [[nodiscard]] bool near_sides(grid::Key leaf) {
        if (on_sides(box_of(leaf, finest_), box_)) {
            return true;
        }
        if (page_->under.empty()) {
            return false;
        }
        if (near_under_.empty()) {
            for (const grid::Key under : page_->under) {
                mark_near(under);
            }
        }
        return near_under_.contains(grid::code(leaf));
    }

    // Marks the nodes over the drawn depth's nodes touching `under`, a page drawn in this one.
    void mark_near(grid::Key under) {
        const grid::Key root = page_->page;
        for (int step = 0; step < 27; ++step) {
            const std::optional<grid::Key> next =
                under.beside((step % 3) - 1, ((step / 3) % 3) - 1, (step / 9) - 1);
            if (!next || !root.contains(*next)) {
                continue;
            }
            for (grid::Key up = *next; up.depth >= root.depth; up = up.parent()) {
                if (!near_under_.insert(grid::code(up)).second || up.depth == root.depth) {
                    break;
                }
            }
        }
    }

    // Whether a seam need look in the node: it's across the page's sides, or touches them.
    [[nodiscard]] bool wanted(const Node& node) {
        return drawn_ == nullptr || node.page != page_ || near_sides(node.key);
    }

    // Cell, face and edge passes after Ju et al., "Dual Contouring of Hermite Data": each edge
    // where solid meets open is met once, as the smallest of the four leaves round it has it.
    // NOLINTNEXTLINE(misc-no-recursion): as deep as the page's levels
    void cell(const Node& node) {
        if (node.is_leaf || node.page != page_ || !wanted(node)) {
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

    // NOLINTNEXTLINE(misc-no-recursion): as deep as the page's levels
    void face(const Node& low, const Node& high, std::size_t axis) {
        if ((low.is_leaf && high.is_leaf) || !(wanted(low) || wanted(high))) {
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
                                              std::size_t along, std::uint32_t half) {
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
        if (std::ranges::none_of(around, [&](const Node& node) { return wanted(node); })) {
            return;
        }
        if (std::ranges::all_of(around, &Node::is_leaf)) {
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
        for (std::size_t i = 0; i < 4; ++i) {
            if (around.at(i).page != nullptr &&
                (smallest == 4 || around.at(i).key.depth > around.at(smallest).key.depth)) {
                smallest = i;
            }
        }
        if (smallest == 4) {
            return;
        }
        const grid::Key owner = around.at(smallest).key;
        const std::uint64_t side = span(owner.depth);
        Place from{owner.x * side, owner.y * side, owner.z * side};
        const auto quarter = static_cast<std::uint64_t>(smallest);
        from.at(across_u.at(axis)) += (1U - (quarter & 1U)) * side;
        from.at(across_v.at(axis)) += (1U - (quarter >> 1U)) * side;
        Place to = from;
        to.at(axis) += side;
        const Corner first = value(from);
        const Corner last = value(to);
        if ((first.out < 0.0) == (last.out < 0.0)) {
            return;
        }
        DVec3 point{static_cast<double>(from[0]), static_cast<double>(from[1]),
                    static_cast<double>(from[2])};
        point[static_cast<glm::length_t>(axis)] +=
            static_cast<double>(side) * first.out / (first.out - last.out);
        std::array<std::uint64_t, 4> codes{};
        for (std::size_t i = 0; i < 4; ++i) {
            const Node& node = around.at(i);
            codes.at(i) = grid::code(node.leaf);
            const auto before = std::span{codes}.first(i);
            if (drawn_ == nullptr && node.page == page_ &&
                std::ranges::find(before, codes.at(i)) == before.end()) {
                Crossings& crossings = crossings_[codes.at(i)];
                crossings.sum += point;
                ++crossings.count;
                crossings.node = node;
            }
        }
        const bool own =
            std::ranges::all_of(around, [&](const Node& node) { return node.page == page_; }) &&
            first.inside && last.inside;
        const bool drawn = drawn_ == nullptr
                               ? own
                               : !own && around[0].page == page_ &&
                                     std::ranges::none_of(around, [](const Node& node) {
                                         return node.page == nullptr;
                                     });
        if (!drawn) {
            return;
        }

        // Solid at the low end faces the edge's way, and round it the order turns anticlockwise.
        constexpr std::array<std::size_t, 4> turning{0, 1, 3, 2};
        Polygon polygon;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t at = turning.at(first.out < 0.0 ? i : 3 - i);
            const std::uint32_t id = vertex(around.at(at));
            if (polygon.count == 0 ||
                (polygon.vertices.at(polygon.count - 1) != id && polygon.vertices[0] != id)) {
                polygon.depths.at(polygon.count) = around.at(at).leaf.depth;
                polygon.vertices.at(polygon.count++) = id;
            }
        }
        if (polygon.count >= 3) {
            polygons_.push_back(polygon);
        }
    }

    std::uint32_t vertex(const Node& node) {
        const auto [found, added] =
            ids_.try_emplace(grid::code(node.leaf), static_cast<std::uint32_t>(nodes_.size()));
        if (added) {
            nodes_.push_back(node);
        }
        return found->second;
    }

    // Of the eight finest nodes round a corner, the `k`th; none past the root's sides.
    [[nodiscard]] std::optional<grid::Key> cell_at(const Place& corner,
                                                   std::uint32_t k) const noexcept {
        const std::uint64_t across = std::uint64_t{1} << finest_;
        Place at{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::uint64_t bit = (k >> axis) & 1U;
            if (corner.at(axis) + bit < 1 || corner.at(axis) + bit > across) {
                return std::nullopt;
            }
            at.at(axis) = corner.at(axis) + bit - 1;
        }
        return grid::Key{.depth = finest_,
                         .x = static_cast<std::uint32_t>(at[0]),
                         .y = static_cast<std::uint32_t>(at[1]),
                         .z = static_cast<std::uint32_t>(at[2])};
    }

    // The page drawing the finest node `cell`, noting where it isn't this one.
    [[nodiscard]] const ShapeDrawn* page_of(grid::Key cell, bool& inside) const {
        const grid::Key root = page_->page;
        if (root.contains(cell) &&
            (page_->under.empty() ||
             !std::ranges::binary_search(page_->under, cell.ancestor(static_cast<std::uint8_t>(
                                                           root.depth + page_->levels))))) {
            return page_;
        }
        inside = false;
        return drawn_ == nullptr ? nullptr : drawn_->over(cell);
    }

    // The mean of where the planes of the smallest drawn leaves touching the corner put it, but
    // solid where it touches solid leaves and no open ones, and open the other way round, so
    // leaves all solid or open have no surface through them. A page's own edges read its own
    // leaves alone, a seam's those either side.
    Corner value(const Place& corner) {
        const std::uint64_t code = corner[0] | (corner[1] << 21U) | (corner[2] << 42U);
        if (const auto known = values_.find(code); known != values_.end()) {
            return known->second;
        }
        std::array<std::uint64_t, 8> seen{};
        std::size_t seen_count = 0;
        int deepest = -1;
        double sum = 0.0;
        int count = 0;
        int solid = 0;
        int open = 0;
        Corner result;
        for (std::uint32_t k = 0; k < 8; ++k) {
            const std::optional<grid::Key> cell = cell_at(corner, k);
            if (!cell) {
                result.inside = false;
                continue;
            }
            const ShapeDrawn* page = page_of(*cell, result.inside);
            if (page == nullptr) {
                continue;
            }
            const grid::Key leaf = make(*cell, page).leaf;
            const std::uint64_t leaf_code = grid::code(leaf);
            const auto earlier = std::span{seen}.first(seen_count);
            if (std::ranges::find(earlier, leaf_code) != earlier.end()) {
                continue;
            }
            seen.at(seen_count++) = leaf_code;
            const Found found = tree_at(*page, leaf);
            solid += found.value.is_solid() ? 1 : 0;
            open += found.value.is_open() ? 1 : 0;
            if (!found.value.crosses()) {
                continue;
            }
            if (int{leaf.depth} > deepest) {
                deepest = leaf.depth;
                sum = 0.0;
                count = 0;
            }
            if (int{leaf.depth} == deepest) {
                sum +=
                    past_plane(*page, leaf, found,
                               DVec3{static_cast<double>(corner[0]), static_cast<double>(corner[1]),
                                     static_cast<double>(corner[2])});
                ++count;
            }
        }
        // Off the corner, so the surface crosses an edge short of it.
        result.out = count > 0 ? sum / count : least_ * (open - solid);
        if (solid > 0 && open == 0) {
            result.out = std::min(result.out, -least_);
        } else if (open > 0 && solid == 0) {
            result.out = std::max(result.out, least_);
        }
        values_.emplace(code, result);
        return result;
    }

    [[nodiscard]] DVec3 middle(grid::Key key) const noexcept {
        return (DVec3{key.x, key.y, key.z} + 0.5) * static_cast<double>(span(key.depth));
    }

    // How far out of the solid `point` lies by the plane of `found`, the tree's leaf over `leaf`.
    [[nodiscard]] double past_plane(const ShapeDrawn& page, grid::Key leaf, const Found& found,
                                    DVec3 point) const noexcept {
        const grid::Key over =
            leaf.ancestor(static_cast<std::uint8_t>(page.page.depth + found.depth));
        return (found.value.distance() * static_cast<double>(span(over.depth))) +
               glm::dot(found.value.normal(), point - middle(over));
    }

    // Where `point` meets the plane of `found`, the tree's leaf over `leaf`, straight along it,
    // kept in the leaf.
    [[nodiscard]] DVec3 on_plane(const ShapeDrawn& page, grid::Key leaf, const Found& found,
                                 DVec3 point) const noexcept {
        point -= found.value.normal() * past_plane(page, leaf, found, point);
        const auto side = static_cast<double>(span(leaf.depth));
        const DVec3 low = DVec3{leaf.x, leaf.y, leaf.z} * side;
        return glm::clamp(point, low, low + side);
    }

    // The mean of where the surface crosses the leaf's own edges, as the leaves round their ends
    // have it, so the same whichever seam asks; its middle where none crosses.
    [[nodiscard]] DVec3 crossed(grid::Key leaf) {
        const std::uint64_t side = span(leaf.depth);
        const Place low{leaf.x * side, leaf.y * side, leaf.z * side};
        std::array<double, 8> out{};
        for (std::uint32_t k = 0; k < 8; ++k) {
            out.at(k) = value({low[0] + ((k & 1U) * side), low[1] + (((k >> 1U) & 1U) * side),
                               low[2] + ((k >> 2U) * side)})
                            .out;
        }
        DVec3 sum{0.0};
        int count = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::uint32_t bit = 1U << axis;
            for (std::uint32_t k = 0; k < 8; ++k) {
                const std::uint32_t far = k | bit;
                if ((k & bit) != 0 || (out.at(k) < 0.0) == (out.at(far) < 0.0)) {
                    continue;
                }
                DVec3 point{static_cast<double>(low[0] + ((k & 1U) * side)),
                            static_cast<double>(low[1] + (((k >> 1U) & 1U) * side)),
                            static_cast<double>(low[2] + ((k >> 2U) * side))};
                point[static_cast<glm::length_t>(axis)] +=
                    static_cast<double>(side) * out.at(k) / (out.at(k) - out.at(far));
                sum += point;
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<double>(count) : middle(leaf);
    }

    // Where a leaf's vertex lies and what it shows: from the mesh of the page it's in, where
    // that has it, else on its plane nearest the crossings round it; a normal of nothing where
    // its plane doesn't cross it. A seam meets only part of a leaf's crossings, and others may
    // meet it too, so where its page hasn't placed it, it's placed by the leaf alone.
    [[nodiscard]] ShapeVertex vertex_of(const Node& node) {
        const std::uint64_t code = grid::code(node.leaf);
        if (drawn_ != nullptr && node.page->mesh != nullptr) {
            if (const auto found = node.page->mesh->border.find(code);
                found != node.page->mesh->border.end()) {
                ShapeVertex vertex = found->second;
                if (node.page != page_) {
                    vertex.position =
                        Vec3{DVec3{vertex.position} + (node.page->mesh->origin - origin_)};
                }
                return vertex;
            }
        }
        const Found found = tree_at(*node.page, node.leaf);
        const auto crossings = drawn_ == nullptr ? crossings_.find(code) : crossings_.end();
        const DVec3 mass =
            crossings == crossings_.end()
                ? crossed(node.leaf)
                : crossings->second.sum / static_cast<double>(crossings->second.count);
        ShapeVertex vertex;
        const DVec3 low{static_cast<double>(box_.low[0]), static_cast<double>(box_.low[1]),
                        static_cast<double>(box_.low[2])};
        if (found.value.crosses()) {
            vertex.position = Vec3{(on_plane(*node.page, node.leaf, found, mass) - low) * metres_};
            vertex.normal = Vec3{found.value.normal()};
            const grid::Blend blend = grid::Blend::of(found.value.material());
            vertex.materials = {blend.first, blend.second};
            vertex.share = static_cast<float>(blend.share);
        } else {
            vertex.position = Vec3{(mass - low) * metres_};
        }
        return vertex;
    }

    const ShapeDrawn* page_;
    const ShapesDrawn* drawn_;
    std::uint8_t finest_;
    Box box_;
    double metres_; // a finest node's side
    DVec3 origin_;
    double least_; // of a drawn node's side, in finest nodes
    std::unordered_map<const ShapeDrawn*, std::unordered_set<std::uint64_t>> holding_;
    std::unordered_set<std::uint64_t> near_under_;
    std::unordered_map<std::uint64_t, Corner> values_;
    std::unordered_map<std::uint64_t, std::uint32_t> ids_;
    std::vector<Node> nodes_;
    std::unordered_map<std::uint64_t, Crossings> crossings_; // by the leaves round them
    std::vector<Polygon> polygons_;
};

} // namespace

ShapeMesh mesh_shape(const grid::Grid& grid, const ShapeDrawn& page) {
    Mesher mesher{grid, page, nullptr};
    mesher.walk();
    return mesher.mesh();
}

ShapeMesh mesh_shape(const grid::Grid& grid, grid::Key root, const grid::Tree<grid::Shape>& tree,
                     std::uint8_t levels) {
    return mesh_shape(
        grid,
        ShapeDrawn{.page = root, .tree = &tree, .levels = levels, .under = {}, .mesh = nullptr});
}

ShapeMesh mesh_seam(const ShapesDrawn& drawn, grid::Key root) {
    const ShapeDrawn* page = drawn.page(root);
    HOG_ASSERT(page != nullptr, "the page drawn");
    Mesher mesher{drawn.grid(), *page, &drawn};
    mesher.walk();
    return mesher.mesh();
}

void ShapesDrawn::add(const ShapeDrawn& page) {
    pages_.insert_or_assign(page.page, page);
}

const ShapeDrawn* ShapesDrawn::page(grid::Key root) const {
    const auto found = pages_.find(root);
    return found == pages_.end() ? nullptr : &found->second;
}

const ShapeDrawn* ShapesDrawn::over(grid::Key node) const {
    grid::Key root =
        node.depth < grid_.depth() ? grid_.page_of(node.child(0)) : grid_.page_of(node);
    while (true) {
        if (const ShapeDrawn* found = page(root)) {
            return found;
        }
        if (root.depth == 0) {
            return nullptr;
        }
        root = grid_.page_of(root);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as pages are drawn in pages
void ShapesDrawn::nested(const ShapeDrawn& page, const std::function<bool(grid::Key)>& wanted,
                         std::unordered_set<grid::Key, Hash>& seen,
                         std::vector<grid::Key>& found) const {
    for (const grid::Key under : page.under) {
        const ShapeDrawn* drawn = this->page(under);
        if (drawn == nullptr || !wanted(under) || !seen.insert(under).second) {
            continue;
        }
        found.push_back(under);
        nested(*drawn, wanted, seen, found);
    }
}

std::vector<grid::Key> ShapesDrawn::touching(grid::Key root) const {
    const std::uint8_t finest = grid_.depth();
    const ShapeDrawn* own = page(root);
    HOG_ASSERT(own != nullptr, "the page drawn");
    std::vector<grid::Key> found;
    std::unordered_set<grid::Key, Hash> seen{root};
    for (const grid::Key under : own->under) {
        // Those in it touch this page only at its sides.
        const Box hole = box_of(under, finest);
        nested(
            ShapeDrawn{.page = root,
                       .tree = nullptr,
                       .levels = 0,
                       .under = std::span{&under, 1},
                       .mesh = nullptr},
            [&](grid::Key inner) {
                return inner == under || on_sides(box_of(inner, finest), hole);
            },
            seen, found);
    }
    const Box box = box_of(root, finest);
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                const std::optional<grid::Key> next = root.beside(dx, dy, dz);
                const ShapeDrawn* drawn = next ? over(*next) : nullptr;
                if (drawn == nullptr) {
                    continue;
                }
                if (seen.insert(drawn->page).second) {
                    found.push_back(drawn->page);
                }
                nested(
                    *drawn, [&](grid::Key inner) { return touch(box_of(inner, finest), box); },
                    seen, found);
            }
        }
    }
    return found;
}

} // namespace hog::client
