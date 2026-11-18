#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/kinds.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::grid {

// One kind of data over a node's subtree, `levels` deep. Every node holds a value: a leaf's
// stands for all under it, a split node's is its kind's summary of its eight. An open leaf has
// finer detail under it, held elsewhere or still to come; a closed one is the same all through.
// Alike nodes are held once, so building the same content gives the same tree. Eight leaves merge
// into one wherever their kind says one value stands for them all.
template <typename Kind>
class Tree {
public:
    using Value = Kind::Value;

    // What's known at a place: the node there, or the leaf over it, and how deep that lies.
    struct Found {
        Value value{};
        bool open = false;
        std::uint8_t depth = 0;

        friend constexpr bool operator==(const Found&, const Found&) noexcept = default;
    };

    static constexpr std::uint8_t format = 1;

    // One leaf over all of it.
    Tree(std::uint8_t levels, Value whole, bool open);

    // Every node at the foot, 2^levels a side, from `value_at(x, y, z)`; closed leaves.
    template <typename ValueAt>
    [[nodiscard]] static Tree build(std::uint8_t levels, const ValueAt& value_at) {
        Tree tree;
        tree.levels_ = levels;
        int per_side = 1 << levels;
        std::vector<Id> level;
        level.reserve(static_cast<std::size_t>(per_side) * static_cast<std::size_t>(per_side) *
                      static_cast<std::size_t>(per_side));
        for (int z = 0; z < per_side; ++z) {
            for (int y = 0; y < per_side; ++y) {
                for (int x = 0; x < per_side; ++x) {
                    level.push_back(tree.leaf_of(value_at(x, y, z), false));
                }
            }
        }
        while (per_side > 1) {
            const int half = per_side / 2;
            const double side = std::ldexp(1.0, levels) / per_side;
            std::vector<Id> coarser;
            coarser.reserve(static_cast<std::size_t>(half) * static_cast<std::size_t>(half) *
                            static_cast<std::size_t>(half));
            const auto at = [&](int x, int y, int z) {
                return level[(((static_cast<std::size_t>(z) * static_cast<std::size_t>(per_side)) +
                               static_cast<std::size_t>(y)) *
                              static_cast<std::size_t>(per_side)) +
                             static_cast<std::size_t>(x)];
            };
            for (int z = 0; z < half; ++z) {
                for (int y = 0; y < half; ++y) {
                    for (int x = 0; x < half; ++x) {
                        std::array<Id, 8> children{};
                        for (std::size_t k = 0; k < 8; ++k) {
                            children.at(k) = at((2 * x) + static_cast<int>(k & 1U),
                                                (2 * y) + static_cast<int>((k >> 1U) & 1U),
                                                (2 * z) + static_cast<int>((k >> 2U) & 1U));
                        }
                        coarser.push_back(tree.joined(children, side));
                    }
                }
            }
            level = std::move(coarser);
            per_side = half;
        }
        tree.root_ = level.front();
        return tree;
    }

    // A node as a builder going down decides it: a leaf with its value, open or not.
    struct Leaf {
        Value value{}; // NOLINT(readability-redundant-member-init): zeroes integral kinds
        bool open = false;
    };

    // From the root down, asking `decide(depth, x, y, z)` of each node reached, x, y, z among
    // that depth's: a leaf ends it there, none splits it. Every node `levels` deep must be a leaf.
    template <typename Decide>
    [[nodiscard]] static Tree grow(std::uint8_t levels, const Decide& decide) {
        struct Frame {
            std::uint8_t depth = 0;
            std::uint32_t x = 0;
            std::uint32_t y = 0;
            std::uint32_t z = 0;
            std::size_t next = 0;
            std::array<Id, 8> children{};
        };
        Tree tree;
        tree.levels_ = levels;
        std::vector<Frame> stack;
        Id done = 0;
        // Pushes a node that splits; a leaf is made at once, as `done`.
        const auto reach = [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                               std::uint32_t z) {
            if (const std::optional<Leaf> leaf = decide(depth, x, y, z)) {
                done = tree.leaf_of(leaf->value, leaf->open);
                return false;
            }
            HOG_ASSERT(depth < levels, "nodes at the foot must be leaves");
            stack.push_back({.depth = depth, .x = x, .y = y, .z = z});
            return true;
        };
        if (!reach(0, 0, 0, 0)) {
            tree.root_ = done;
            return tree;
        }
        while (!stack.empty()) {
            const Frame top = stack.back();
            if (top.next == 8) {
                done = tree.joined(top.children, std::ldexp(1.0, levels - top.depth - 1));
                stack.pop_back();
                if (stack.empty()) {
                    break;
                }
                Frame& parent = stack.back();
                parent.children.at(parent.next++) = done;
                continue;
            }
            const auto octant = static_cast<std::uint32_t>(top.next);
            if (!reach(static_cast<std::uint8_t>(top.depth + 1), (2 * top.x) + (octant & 1U),
                       (2 * top.y) + ((octant >> 1U) & 1U), (2 * top.z) + ((octant >> 2U) & 1U))) {
                Frame& same = stack.back();
                same.children.at(same.next++) = done;
            }
        }
        tree.root_ = done;
        return tree;
    }

    [[nodiscard]] std::uint8_t levels() const noexcept { return levels_; }

    // Cut to `levels` deep, where it's deeper: a node there that splits becomes an open leaf of its
    // value.
    [[nodiscard]] Tree trimmed(std::uint8_t levels) const {
        if (levels >= levels_) {
            return *this;
        }
        return grow(levels,
                    [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                        std::uint32_t z) -> std::optional<Leaf> {
                        const Found found = at(depth, x, y, z);
                        if (at(depth + 1, 2 * x, 2 * y, 2 * z).depth == depth) {
                            return Leaf{.value = found.value, .open = found.open};
                        }
                        if (depth == levels) {
                            return Leaf{.value = found.value, .open = true};
                        }
                        return std::nullopt;
                    });
    }

    // The node `depth` below the root at (x, y, z) among that depth's, or the leaf over it.
    // Precondition: depth <= levels and x, y, z < 2^depth.
    [[nodiscard]] Found at(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                           std::uint32_t z) const noexcept;
    [[nodiscard]] Found root() const noexcept { return at(0, 0, 0, 0); }

    // Distinct nodes, and leaves among them, reachable from the root.
    [[nodiscard]] std::size_t nodes() const;
    [[nodiscard]] std::size_t leaves() const;
    // The value of each distinct leaf reachable from the root; an open and a closed one alike
    // give it twice.
    [[nodiscard]] std::vector<Value> leaf_values() const;

    // Little-endian: format, levels, a varint node count, then each node once, after its
    // children: flags (1 split, 2 open), its value, and a split node's eight children as varint
    // indices. The root comes last.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a tree, go deeper than they say, or hold a split
    // node whose value isn't its children's summary.
    [[nodiscard]] static Result<Tree> decode(std::span<const std::byte> bytes);

    [[nodiscard]] bool same_as(const Tree& other) const { return encode() == other.encode(); }

private:
    using Id = std::uint32_t;
    static constexpr Id none = std::numeric_limits<Id>::max();

    struct Node {
        Value value{}; // NOLINT(readability-redundant-member-init): zeroes integral kinds
        bool open = false;
        std::array<Id, 8> children{none, none, none, none, none, none, none, none};

        [[nodiscard]] bool split() const noexcept { return children[0] != none; }
        friend bool operator==(const Node&, const Node&) noexcept = default;
    };
    struct Hash {
        std::size_t operator()(const Node& node) const noexcept;
    };

    Tree() = default;

    [[nodiscard]] Id held(const Node& node);
    [[nodiscard]] Id leaf_of(Value value, bool opened) {
        return held({.value = value, .open = opened});
    }
    // Eight leaves alike in being open or closed become one where their kind merges them; else a
    // split node. Of children `side` finest nodes across.
    [[nodiscard]] Id joined(const std::array<Id, 8>& children, double side);
    // A decoded node's id here and how far it reaches down.
    struct Decoded {
        Id id = 0;
        std::uint8_t reach = 0;
    };
    // One node, its children among those already decoded.
    [[nodiscard]] Result<Decoded> decode_node(ByteReader& in, std::span<const Decoded> decoded);
    // Reachable ids, each after its children.
    [[nodiscard]] std::vector<Id> walk() const;

    std::vector<Node> nodes_;
    std::unordered_map<Node, Id, Hash> ids_;
    Id root_ = 0;
    std::uint8_t levels_ = 0;
};

extern template class Tree<Shape>;
extern template class Tree<Material>;
extern template class Tree<Water>;

} // namespace hog::grid
