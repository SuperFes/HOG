#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace hog::globe {

namespace {

// One of a cube's matters over eight voxels: the material holding most of it, and its average.
struct Share {
    std::array<std::uint32_t, material_count> by{};

    void add(Material material, std::uint16_t amount) {
        by.at(static_cast<std::size_t>(material)) += amount;
    }
    [[nodiscard]] std::pair<Material, std::uint16_t> taken() const {
        // Air holds nothing of a matter, so is never the most of one.
        std::size_t most = 1;
        std::uint32_t total = 0;
        for (std::size_t k = 1; k < by.size(); ++k) {
            total += by.at(k);
            if (by.at(k) > by.at(most)) {
                most = k;
            }
        }
        const auto amount = static_cast<std::uint16_t>((total + 4) / 8);
        return amount == 0 ? std::pair{Material::Air, std::uint16_t{0}}
                           : std::pair{static_cast<Material>(most), amount};
    }
};

Cube summed(const std::array<Cube, 8>& eight) {
    Share solid;
    Share grain;
    Share liquid;
    std::uint32_t heat = 0;
    std::uint8_t condition = Voxel::sound;
    for (const Cube& cube : eight) {
        solid.add(cube.solid, cube.solid_amount);
        grain.add(cube.grain, cube.grain_amount);
        liquid.add(cube.liquid, cube.liquid_amount);
        heat += cube.heat;
        condition = std::min(condition, cube.condition);
    }
    Cube out;
    std::tie(out.solid, out.solid_amount) = solid.taken();
    std::tie(out.grain, out.grain_amount) = grain.taken();
    std::tie(out.liquid, out.liquid_amount) = liquid.taken();
    out.heat = static_cast<std::uint8_t>((heat + 4) / 8);
    out.condition = out.solid == Material::Air ? Voxel::sound : condition;
    return out;
}

} // namespace

std::size_t VoxelTree::Hash::operator()(const Node& node) const noexcept {
    if (node.leaf) {
        const Cube& v = node.voxel;
        return hash_values(1, static_cast<int>(v.solid), v.solid_amount, static_cast<int>(v.grain),
                           v.grain_amount, static_cast<int>(v.liquid), v.liquid_amount, v.condition,
                           v.heat, node.open);
    }
    return hash_values(2, node.child[0], node.child[1], node.child[2], node.child[3], node.child[4],
                       node.child[5], node.child[6], node.child[7]);
}

bool VoxelTree::Equal::operator()(const Node& a, const Node& b) const noexcept {
    if (a.leaf != b.leaf) {
        return false;
    }
    return a.leaf ? a.voxel == b.voxel && a.open == b.open : a.child == b.child;
}

VoxelTree::VoxelTree(int side, const Cube& fill, std::uint8_t open) : side_{side} {
    HOG_ASSERT(side > 0 && (side & (side - 1)) == 0, "a tree's side is a power of two");
    root_ = leaf_of(fill, open);
}

VoxelTree::Id VoxelTree::intern(const Node& node) {
    const auto [found, added] = ids_.try_emplace(node, static_cast<Id>(nodes_.size()));
    if (added) {
        nodes_.push_back(node);
    }
    return found->second;
}

VoxelTree::Id VoxelTree::leaf_of(const Cube& voxel, std::uint8_t open) {
    return intern({.leaf = true, .voxel = voxel, .child = {}, .open = open});
}

VoxelTree::Id VoxelTree::joined(const std::array<Id, 8>& children) {
    const Node& first = nodes_[children[0]];
    bool alike = first.leaf;
    for (const Id child : children) {
        alike = alike && child == children[0];
    }
    if (alike) {
        return children[0];
    }
    std::array<Cube, 8> eight;
    std::uint8_t open = 0;
    for (std::size_t k = 0; k < eight.size(); ++k) {
        eight.at(k) = nodes_[children.at(k)].voxel;
        open = std::max(open, nodes_[children.at(k)].open);
    }
    return intern({.leaf = false, .voxel = summed(eight), .child = children, .open = open});
}

Cube VoxelTree::at(int x, int y, int z) const {
    HOG_ASSERT(x >= 0 && y >= 0 && z >= 0 && x < side_ && y < side_ && z < side_,
               "a point in the tree");
    Id node = root_;
    int size = side_;
    while (!nodes_[node].leaf) {
        const int half = size / 2;
        const std::size_t k = (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        node = nodes_[node].child[k];
        x %= half;
        y %= half;
        z %= half;
        size = half;
    }
    return nodes_[node].voxel;
}

Cube VoxelTree::average(int x, int y, int z, int across) const {
    HOG_ASSERT(x >= 0 && y >= 0 && z >= 0 && x < side_ && y < side_ && z < side_,
               "a point in the tree");
    HOG_ASSERT(across > 0 && across <= side_ && (across & (across - 1)) == 0,
               "a cube a power of two across, no wider than the tree");
    return nodes_[node_at(x, y, z, across).first].voxel;
}

std::pair<VoxelTree::Id, int> VoxelTree::node_at(int x, int y, int z, int across) const {
    Id node = root_;
    int size = side_;
    while (!nodes_[node].leaf && size > across) {
        const int half = size / 2;
        const std::size_t k = (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        node = nodes_[node].child[k];
        x %= half;
        y %= half;
        z %= half;
        size = half;
    }
    return {node, size};
}

int VoxelTree::leaf_side(int x, int y, int z) const {
    Id node = root_;
    int size = side_;
    while (!nodes_[node].leaf) {
        const int half = size / 2;
        const std::size_t k = (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        node = nodes_[node].child[k];
        x %= half;
        y %= half;
        z %= half;
        size = half;
    }
    return size;
}

VoxelTree::Id VoxelTree::with(Id node, int side, int x, int y, int z, const Cube& voxel) {
    // Down to the voxel's own place or the first leaf already holding it, noting each cube passed
    // and which of its eight we went into; then back up, making a node of each with its one new
    // child, and joining where they come alike.
    std::vector<Step> path;
    Id replacement = node;
    while (true) {
        const Node here = nodes_[node];
        HOG_ASSERT(!here.leaf || here.open == 0, "a voxel set where the ground is made finely");
        if (here.leaf && here.voxel == voxel) {
            replacement = node;
            break;
        }
        if (side == 1) {
            replacement = leaf_of(voxel);
            break;
        }
        const int half = side / 2;
        const std::size_t k = (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        path.push_back({.node = node, .child = k});
        // A leaf is eight leaves alike, so a cube split from one goes on into itself.
        node = here.leaf ? node : here.child[k];
        x %= half;
        y %= half;
        z %= half;
        side = half;
    }
    return rejoined(path, replacement);
}

VoxelTree::Id VoxelTree::rejoined(const std::vector<Step>& path, Id replacement) {
    for (auto step = path.rbegin(); step != path.rend(); ++step) {
        const Node above = nodes_[step->node];
        std::array<Id, 8> children =
            above.leaf ? std::array<Id, 8>{step->node, step->node, step->node, step->node,
                                           step->node, step->node, step->node, step->node}
                       : above.child;
        children.at(step->child) = replacement;
        replacement = joined(children);
    }
    return replacement;
}

void VoxelTree::set(int x, int y, int z, const Cube& voxel) {
    HOG_ASSERT(x >= 0 && y >= 0 && z >= 0 && x < side_ && y < side_ && z < side_,
               "a point in the tree");
    root_ = with(root_, side_, x, y, z, voxel);
}

VoxelTree VoxelTree::window(int x, int y, int z, int across, int voxel, std::uint8_t cut) const {
    HOG_ASSERT(voxel > 0 && voxel <= across && across <= side_ && (voxel & (voxel - 1)) == 0 &&
                   (across & (across - 1)) == 0,
               "a window and its voxels powers of two, no wider than the tree");
    HOG_ASSERT(x >= 0 && y >= 0 && z >= 0 && x % across == 0 && y % across == 0 &&
                   z % across == 0 && x < side_ && y < side_ && z < side_,
               "a window within the tree, on its grid");
    VoxelTree out{across / voxel};
    // Each node with its size, as nodes alike may stand at different sizes and be cut differently.
    std::unordered_map<std::uint64_t, Id> made;
    const auto key = [](Id node, int size) {
        return (std::uint64_t{node} << 32U) | static_cast<std::uint32_t>(size);
    };
    struct Open {
        Id node;
        int size;
        std::size_t next = 0;
        std::array<Id, 8> children{};
    };
    std::vector<Open> open;
    const auto done = [&](Id node, int size) -> std::optional<Id> {
        const Node& here = nodes_[node];
        if (here.leaf) {
            return out.leaf_of(here.voxel, here.open);
        }
        if (size <= voxel) {
            return out.leaf_of(here.voxel, std::max(cut, here.open));
        }
        if (const auto found = made.find(key(node, size)); found != made.end()) {
            return found->second;
        }
        return std::nullopt;
    };
    const auto [top, top_size] = node_at(x, y, z, across);
    if (const auto id = done(top, top_size)) {
        out.root_ = *id;
        return out;
    }
    open.push_back({.node = top, .size = top_size});
    while (true) {
        Open& last = open.back();
        if (last.next == 8) {
            const Id id = out.joined(last.children);
            made.emplace(key(last.node, last.size), id);
            open.pop_back();
            if (open.empty()) {
                out.root_ = id;
                return out;
            }
            open.back().children.at(open.back().next++) = id;
            continue;
        }
        const Id child = nodes_[last.node].child.at(last.next);
        const int half = last.size / 2;
        if (const auto id = done(child, half)) {
            last.children.at(last.next++) = *id;
        } else {
            open.push_back({.node = child, .size = half});
        }
    }
}

std::uint8_t VoxelTree::coarsest(int x, int y, int z, int across) const {
    return nodes_[node_at(x, y, z, across).first].open;
}

VoxelTree::Id VoxelTree::imported(const VoxelTree& part, Id node, std::uint8_t open) {
    std::unordered_map<Id, Id> made;
    struct Open {
        Id node;
        std::size_t next = 0;
        std::array<Id, 8> children{};
    };
    const auto done = [&](Id at) -> std::optional<Id> {
        const Node& here = part.nodes_[at];
        if (here.leaf) {
            return leaf_of(here.voxel, std::max(here.open, open));
        }
        if (const auto found = made.find(at); found != made.end()) {
            return found->second;
        }
        return std::nullopt;
    };
    if (const auto id = done(node)) {
        return *id;
    }
    std::vector<Open> pending{{.node = node}};
    while (true) {
        Open& last = pending.back();
        if (last.next == 8) {
            const Id id = joined(last.children);
            made.emplace(last.node, id);
            pending.pop_back();
            if (pending.empty()) {
                return id;
            }
            pending.back().children.at(pending.back().next++) = id;
            continue;
        }
        const Id child = part.nodes_[last.node].child.at(last.next);
        if (const auto id = done(child)) {
            last.children.at(last.next++) = *id;
        } else {
            pending.push_back({.node = child});
        }
    }
}

void VoxelTree::graft(int x, int y, int z, const VoxelTree& part, int voxel, std::uint8_t open) {
    const int across = part.side() * voxel;
    HOG_ASSERT(across <= side_ && x >= 0 && y >= 0 && z >= 0 && x % across == 0 &&
                   y % across == 0 && z % across == 0 && x < side_ && y < side_ && z < side_,
               "a part within the tree, on its grid");
    // Down to the part's cube, as `with` goes.
    std::vector<Step> path;
    Id node = root_;
    for (int side = side_; side > across; side /= 2) {
        const int half = side / 2;
        const std::size_t k = (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        path.push_back({.node = node, .child = k});
        node = nodes_[node].leaf ? node : nodes_[node].child[k];
        x %= half;
        y %= half;
        z %= half;
    }
    root_ = rejoined(path, merged(node, part, open));
}

VoxelTree::Id VoxelTree::merged(Id node, const VoxelTree& part, std::uint8_t open) {
    // This tree's nodes and the part's side by side: a leaf here marked coarser than the part is
    // the part's from there down, and anything else here is kept, its children gone through.
    struct Open {
        Id here;
        Id there;
        std::size_t next = 0;
        std::array<Id, 8> children{};
    };
    const auto done = [&](Id here, Id there) -> std::optional<Id> {
        const Node& mine = nodes_[here];
        if (mine.open <= open) {
            return here;
        }
        if (!mine.leaf) {
            return std::nullopt;
        }
        return imported(part, there, open);
    };
    if (const auto id = done(node, part.root_)) {
        return *id;
    }
    std::vector<Open> pending{{.here = node, .there = part.root_}};
    while (true) {
        Open& last = pending.back();
        if (last.next == 8) {
            const Id id = joined(last.children);
            pending.pop_back();
            if (pending.empty()) {
                return id;
            }
            pending.back().children.at(pending.back().next++) = id;
            continue;
        }
        const Id here = nodes_[last.here].child.at(last.next);
        const Node& theirs = part.nodes_[last.there];
        const Id there = theirs.leaf ? last.there : theirs.child.at(last.next);
        if (const auto id = done(here, there)) {
            last.children.at(last.next++) = *id;
        } else {
            pending.push_back({.here = here, .there = there});
        }
    }
}

std::size_t VoxelTree::nodes() const {
    std::unordered_set<Id> seen;
    std::vector<Id> pending{root_};
    while (!pending.empty()) {
        const Id node = pending.back();
        pending.pop_back();
        if (!seen.insert(node).second) {
            continue;
        }
        if (!nodes_[node].leaf) {
            for (const Id child : nodes_[node].child) {
                pending.push_back(child);
            }
        }
    }
    return seen.size();
}

std::size_t VoxelTree::unshared_nodes() const {
    std::size_t count = 0;
    std::vector<Id> pending{root_};
    while (!pending.empty()) {
        const Id node = pending.back();
        pending.pop_back();
        ++count;
        if (!nodes_[node].leaf) {
            for (const Id child : nodes_[node].child) {
                pending.push_back(child);
            }
        }
    }
    return count;
}

std::size_t VoxelTree::leaves() const {
    std::size_t count = 0;
    std::vector<Id> pending{root_};
    while (!pending.empty()) {
        const Id node = pending.back();
        pending.pop_back();
        if (nodes_[node].leaf) {
            ++count;
        } else {
            for (const Id child : nodes_[node].child) {
                pending.push_back(child);
            }
        }
    }
    return count;
}

namespace {

// Bytes that mark a node as a leaf, a cube of eight, or a leaf made coarsely, its mark next.
constexpr std::uint8_t leaf_tag = 0;
constexpr std::uint8_t cube_tag = 1;
constexpr std::uint8_t open_tag = 2;
// The most nodes a tree of bytes may claim, so damaged bytes can't ask for the memory of a planet.
constexpr std::uint64_t most_nodes = std::uint64_t{1} << 28U;

// A leaf's voxel from its bytes; none where they're cut short or out of range.
std::optional<Cube> read_cube(ByteReader& in) {
    const auto material = [](Result<std::uint8_t> value) -> std::optional<Material> {
        if (!value || *value >= material_count) {
            return std::nullopt;
        }
        return static_cast<Material>(*value);
    };
    const auto solid = material(in.read<std::uint8_t>());
    const auto solid_amount = in.read<std::uint16_t>();
    const auto grain = material(in.read<std::uint8_t>());
    const auto grain_amount = in.read<std::uint16_t>();
    const auto liquid = material(in.read<std::uint8_t>());
    const auto liquid_amount = in.read<std::uint16_t>();
    const auto condition = in.read<std::uint8_t>();
    const auto heat = in.read<std::uint8_t>();
    if (!solid || !solid_amount || !grain || !grain_amount || !liquid || !liquid_amount ||
        !condition || !heat) {
        return std::nullopt;
    }
    return Cube{.solid = *solid,
                .solid_amount = *solid_amount,
                .grain = *grain,
                .grain_amount = *grain_amount,
                .liquid = *liquid,
                .liquid_amount = *liquid_amount,
                .condition = *condition,
                .heat = *heat};
}

} // namespace

std::vector<std::byte> VoxelTree::encode() const {
    // The reachable nodes in the order a walk down from the root, children in turn, finishes them:
    // children before parents, and the same for the same voxels however the nodes were made.
    std::vector<Id> order;
    std::unordered_map<Id, std::uint64_t> dense;
    {
        struct Walk {
            Id node;
            std::size_t next;
        };
        std::vector<Walk> pending{{.node = root_, .next = 0}};
        while (!pending.empty()) {
            Walk& walk = pending.back();
            const Node& node = nodes_[walk.node];
            if (node.leaf || walk.next == node.child.size()) {
                if (dense.try_emplace(walk.node, order.size()).second) {
                    order.push_back(walk.node);
                }
                pending.pop_back();
                continue;
            }
            const Id child = node.child[walk.next++];
            if (!dense.contains(child)) {
                pending.push_back({.node = child, .next = 0});
            }
        }
    }
    ByteWriter out;
    out.write_varint(static_cast<std::uint64_t>(side_));
    out.write_varint(order.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Node& node = nodes_[order[k]];
        if (node.leaf) {
            const Cube& v = node.voxel;
            if (node.open == 0) {
                out.write(leaf_tag);
            } else {
                out.write(open_tag);
                out.write(node.open);
            }
            out.write(static_cast<std::uint8_t>(v.solid));
            out.write(v.solid_amount);
            out.write(static_cast<std::uint8_t>(v.grain));
            out.write(v.grain_amount);
            out.write(static_cast<std::uint8_t>(v.liquid));
            out.write(v.liquid_amount);
            out.write(v.condition);
            out.write(v.heat);
        } else {
            out.write(cube_tag);
            for (const Id child : node.child) {
                // How far back the child is, which is small where cubes are made together.
                out.write_varint(k - dense.at(child));
            }
        }
    }
    return std::move(out).take();
}

Result<VoxelTree> VoxelTree::decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto fail = [](std::string_view why) {
        return make_error(ErrorCode::InvalidArgument, std::string{why});
    };
    const auto side = in.read_varint();
    const auto count = in.read_varint();
    if (!side || !count) {
        return fail("a tree's bytes are cut short");
    }
    if (*side == 0 || (*side & (*side - 1)) != 0 || *side > (std::uint64_t{1} << 20U)) {
        return fail("a tree's side must be a power of two");
    }
    if (*count == 0 || *count > most_nodes) {
        return fail("a tree's node count is out of range");
    }
    VoxelTree tree{static_cast<int>(*side)};
    std::vector<Id> ids;
    ids.reserve(static_cast<std::size_t>(*count));
    for (std::uint64_t k = 0; k < *count; ++k) {
        const auto tag = in.read<std::uint8_t>();
        if (!tag) {
            return fail("a tree's bytes are cut short");
        }
        if (*tag == leaf_tag || *tag == open_tag) {
            const auto open =
                *tag == open_tag ? in.read<std::uint8_t>() : Result<std::uint8_t>{std::uint8_t{0}};
            const std::optional<Cube> voxel = read_cube(in);
            if (!open || (*tag == open_tag && *open == 0) || !voxel) {
                return fail("a leaf is cut short or has a value out of range");
            }
            ids.push_back(tree.leaf_of(*voxel, *open));
        } else if (*tag == cube_tag) {
            std::array<Id, 8> children{};
            for (Id& child : children) {
                const auto back = in.read_varint();
                if (!back || *back == 0 || *back > k) {
                    return fail("a cube's child is not an earlier node");
                }
                child = ids[static_cast<std::size_t>(k - *back)];
            }
            // Bytes made by encode never hold eight leaves alike; refusing them keeps one form.
            const Id joined = tree.joined(children);
            if (tree.nodes_[joined].leaf) {
                return fail("a cube of eight alike leaves is not in its joined form");
            }
            ids.push_back(joined);
        } else {
            return fail("a node is neither a leaf nor a cube");
        }
    }
    if (const auto done = in.finish(); !done) {
        return fail("a tree's bytes run on past its nodes");
    }
    tree.root_ = ids.back();
    return tree;
}

bool VoxelTree::same_as(const VoxelTree& other) const {
    if (side_ != other.side_) {
        return false;
    }
    std::vector<std::pair<Id, Id>> pending{{root_, other.root_}};
    while (!pending.empty()) {
        const auto [a, b] = pending.back();
        pending.pop_back();
        const Node& left = nodes_[a];
        const Node& right = other.nodes_[b];
        if (left.leaf != right.leaf) {
            return false;
        }
        if (left.leaf) {
            if (!(left.voxel == right.voxel) || left.open != right.open) {
                return false;
            }
            continue;
        }
        for (std::size_t k = 0; k < 8; ++k) {
            pending.emplace_back(left.child[k], right.child[k]);
        }
    }
    return true;
}

VoxelTree halved(const VoxelTree& tree) {
    HOG_ASSERT(tree.side() > 1, "a tree wider than one voxel");
    // A cube within one leaf of the finer tree is that leaf's voxel all through.
    const auto whole = [&](int x, int y, int z, int across) -> std::optional<Cube> {
        if (tree.leaf_side(2 * x, 2 * y, 2 * z) >= 2 * across) {
            std::array<Cube, 8> eight;
            eight.fill(tree.at(2 * x, 2 * y, 2 * z));
            return summed(eight);
        }
        return std::nullopt;
    };
    const auto voxel_at = [&](int x, int y, int z) { return tree.average(2 * x, 2 * y, 2 * z, 2); };
    return VoxelTree::build_down(tree.side() / 2, whole, voxel_at);
}

VoxelTree parent_of(const std::array<Octant, 8>& children, int side, const VoxelTree* kept) {
    HOG_ASSERT(side > 1, "a tree wider than one voxel");
    const int half = side / 2;
    const auto octant = [&](int x, int y, int z) -> const Octant& {
        const std::size_t index =
            (x >= half ? 1U : 0U) + (y >= half ? 2U : 0U) + (z >= half ? 4U : 0U);
        return children.at(index);
    };
    // A cube within one child, where that's all one voxel or within one leaf of it.
    const auto whole = [&](int x, int y, int z, int across) -> std::optional<Cube> {
        if (across > half) {
            return std::nullopt;
        }
        const Octant& child = octant(x, y, z);
        if (child.tree != nullptr) {
            const int at_x = x % half;
            const int at_y = y % half;
            const int at_z = z % half;
            return child.tree->leaf_side(2 * at_x, 2 * at_y, 2 * at_z) >= 2 * across
                       ? std::optional{child.tree->average(2 * at_x, 2 * at_y, 2 * at_z, 2)}
                       : std::nullopt;
        }
        if (kept != nullptr) {
            return kept->leaf_side(x, y, z) >= across ? std::optional{kept->at(x, y, z)}
                                                      : std::nullopt;
        }
        return child.fill;
    };
    const auto voxel_at = [&](int x, int y, int z) {
        const Octant& child = octant(x, y, z);
        if (child.tree != nullptr) {
            return child.tree->average(2 * (x % half), 2 * (y % half), 2 * (z % half), 2);
        }
        return kept != nullptr ? kept->at(x, y, z) : child.fill;
    };
    return VoxelTree::build_down(side, whole, voxel_at);
}

} // namespace hog::globe
