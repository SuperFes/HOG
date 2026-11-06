#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <unordered_set>

namespace hog::globe {

std::size_t VoxelTree::Hash::operator()(const Node& node) const noexcept {
    if (node.leaf) {
        const Voxel& v = node.voxel;
        return hash_values(1, v.distance, v.normal, static_cast<int>(v.solid), v.condition,
                           static_cast<int>(v.grain), v.grain_amount, static_cast<int>(v.liquid),
                           v.liquid_amount, v.temperature, v.solid_amount);
    }
    return hash_values(2, node.child[0], node.child[1], node.child[2], node.child[3],
                       node.child[4], node.child[5], node.child[6], node.child[7]);
}

bool VoxelTree::Equal::operator()(const Node& a, const Node& b) const noexcept {
    if (a.leaf != b.leaf) {
        return false;
    }
    return a.leaf ? a.voxel == b.voxel : a.child == b.child;
}

VoxelTree::VoxelTree(int side, const Voxel& fill) : side_{side} {
    HOG_ASSERT(side > 0 && (side & (side - 1)) == 0, "a tree's side is a power of two");
    root_ = leaf_of(fill);
}

VoxelTree::Id VoxelTree::intern(const Node& node) {
    const auto [found, added] = ids_.try_emplace(node, static_cast<Id>(nodes_.size()));
    if (added) {
        nodes_.push_back(node);
    }
    return found->second;
}

VoxelTree::Id VoxelTree::leaf_of(const Voxel& voxel) {
    return intern({.leaf = true, .voxel = voxel, .child = {}});
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
    return intern({.leaf = false, .voxel = {}, .child = children});
}

Voxel VoxelTree::at(int x, int y, int z) const {
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

VoxelTree::Id VoxelTree::with(Id node, int side, int x, int y, int z, const Voxel& voxel) {
    // Down to the voxel's own place or the first leaf already holding it, noting each cube passed
    // and which of its eight we went into; then back up, making a node of each with its one new
    // child, and joining where they come alike.
    struct Step {
        Id node;
        std::size_t child;
    };
    std::vector<Step> path;
    Id replacement = node;
    while (true) {
        const Node here = nodes_[node];
        if (here.leaf && here.voxel == voxel) {
            replacement = node;
            break;
        }
        if (side == 1) {
            replacement = leaf_of(voxel);
            break;
        }
        const int half = side / 2;
        const std::size_t k =
            (x >= half ? 1U : 0U) | (y >= half ? 2U : 0U) | (z >= half ? 4U : 0U);
        path.push_back({.node = node, .child = k});
        // A leaf is eight leaves alike, so a cube split from one goes on into itself.
        node = here.leaf ? node : here.child[k];
        x %= half;
        y %= half;
        z %= half;
        side = half;
    }
    for (auto step = path.rbegin(); step != path.rend(); ++step) {
        const Node above = nodes_[step->node];
        std::array<Id, 8> children = above.leaf ? std::array<Id, 8>{step->node, step->node,
                                                                    step->node, step->node,
                                                                    step->node, step->node,
                                                                    step->node, step->node}
                                                : above.child;
        children.at(step->child) = replacement;
        replacement = joined(children);
    }
    return replacement;
}

void VoxelTree::set(int x, int y, int z, const Voxel& voxel) {
    HOG_ASSERT(x >= 0 && y >= 0 && z >= 0 && x < side_ && y < side_ && z < side_,
               "a point in the tree");
    root_ = with(root_, side_, x, y, z, voxel);
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
            if (!(left.voxel == right.voxel)) {
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

} // namespace hog::globe
