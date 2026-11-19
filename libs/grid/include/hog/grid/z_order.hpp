#pragma once

#include <hog/grid/key.hpp>

#include <cstdint>
#include <map>

namespace hog::grid {

// A node's Morton bits moved up to the deepest a key goes, so a node and everything under it lie
// together in order, the node first.
[[nodiscard]] constexpr std::uint64_t aligned(Key key) noexcept {
    return morton(key) << (3U * (max_depth - key.depth));
}

// The last aligned code under the node.
[[nodiscard]] constexpr std::uint64_t aligned_end(Key key) noexcept {
    return aligned(key) + ((std::uint64_t{1} << (3U * (max_depth - key.depth))) - 1U);
}

// Nodes in Z order, each just before those under it.
struct ZOrder {
    [[nodiscard]] constexpr bool operator()(Key a, Key b) const noexcept {
        const std::uint64_t left = aligned(a);
        const std::uint64_t right = aligned(b);
        return left != right ? left < right : a.depth < b.depth;
    }
};

template <typename Value>
using ZMap = std::map<Key, Value, ZOrder>;

// Calls visit(key, value) for each entry of `map` over `node`, at it or under it, those over it
// first, the rest in Z order. `visit` mustn't add or remove entries.
template <typename Map, typename Visit>
void for_nested(Map& map, Key node, const Visit& visit) {
    for (std::uint8_t depth = 0; depth < node.depth; ++depth) {
        if (const auto found = map.find(node.ancestor(depth)); found != map.end()) {
            visit(found->first, found->second);
        }
    }
    const std::uint64_t end = aligned_end(node);
    for (auto at = map.lower_bound(node); at != map.end() && aligned(at->first) <= end; ++at) {
        visit(at->first, at->second);
    }
}

} // namespace hog::grid
