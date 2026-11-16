#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace hog::grid {

namespace {

// The most common of the eight, the lowest of those tied.
template <typename Value>
Value most(const std::array<Value, 8>& children) noexcept {
    Value best = children[0];
    int best_count = 0;
    for (const Value candidate : children) {
        const auto count = static_cast<int>(std::ranges::count(children, candidate));
        if (count > best_count || (count == best_count && candidate < best)) {
            best = candidate;
            best_count = count;
        }
    }
    return best;
}

constexpr std::uint8_t split_flag = 1;
constexpr std::uint8_t open_flag = 2;

} // namespace

Shape::Value Shape::summary(const std::array<Value, 8>& children) noexcept {
    std::uint32_t sum = 4;
    for (const Value child : children) {
        sum += child;
    }
    return static_cast<Value>(sum / 8);
}

Material::Value Material::summary(const std::array<Value, 8>& children) noexcept {
    return most(children);
}

Water::Value Water::summary(const std::array<Value, 8>& children) noexcept {
    return most(children);
}

template <typename Kind>
Tree<Kind>::Tree(std::uint8_t levels, Value whole, bool open)
    : root_(leaf_of(whole, open)), levels_(levels) {}

template <typename Kind>
std::size_t Tree<Kind>::Hash::operator()(const Node& node) const noexcept {
    std::uint64_t hash = hash_values(0, node.value, node.open);
    for (const Id child : node.children) {
        hash = hash_combine(hash, child);
    }
    return static_cast<std::size_t>(hash);
}

template <typename Kind>
auto Tree<Kind>::held(const Node& node) -> Id {
    const auto [found, added] = ids_.try_emplace(node, static_cast<Id>(nodes_.size()));
    if (added) {
        nodes_.push_back(node);
    }
    return found->second;
}

template <typename Kind>
auto Tree<Kind>::joined(const std::array<Id, 8>& children) -> Id {
    if (std::ranges::all_of(children, [&](Id child) { return child == children[0]; }) &&
        !nodes_[children[0]].split()) {
        return children[0];
    }
    std::array<Value, 8> values{};
    for (std::size_t k = 0; k < 8; ++k) {
        values.at(k) = nodes_[children.at(k)].value;
    }
    return held({.value = Kind::summary(values), .open = false, .children = children});
}

template <typename Kind>
auto Tree<Kind>::at(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                    std::uint32_t z) const noexcept -> Found {
    Id id = root_;
    for (std::uint8_t down = 0; down < depth; ++down) {
        const Node& node = nodes_[id];
        if (!node.split()) {
            return {.value = node.value, .open = node.open, .depth = down};
        }
        const auto shift = static_cast<unsigned>(depth - down - 1);
        const std::size_t octant =
            ((x >> shift) & 1U) | (((y >> shift) & 1U) << 1U) | (((z >> shift) & 1U) << 2U);
        id = node.children.at(octant);
    }
    const Node& node = nodes_[id];
    return {.value = node.value, .open = node.open, .depth = depth};
}

template <typename Kind>
auto Tree<Kind>::walk() const -> std::vector<Id> {
    std::vector<Id> order;
    std::vector<bool> seen(nodes_.size(), false);
    // Each entry: a node and the next of its children to visit.
    std::vector<std::pair<Id, std::size_t>> stack{{root_, 0}};
    seen[root_] = true;
    while (!stack.empty()) {
        auto& [id, next] = stack.back();
        const Node& node = nodes_[id];
        if (node.split() && next < 8) {
            const Id child = node.children.at(next++);
            if (!seen[child]) {
                seen[child] = true;
                stack.emplace_back(child, 0);
            }
            continue;
        }
        order.push_back(id);
        stack.pop_back();
    }
    return order;
}

template <typename Kind>
std::size_t Tree<Kind>::nodes() const {
    return walk().size();
}

template <typename Kind>
std::size_t Tree<Kind>::leaves() const {
    const std::vector<Id> order = walk();
    return static_cast<std::size_t>(
        std::ranges::count_if(order, [&](Id id) { return !nodes_[id].split(); }));
}

template <typename Kind>
std::vector<std::byte> Tree<Kind>::encode() const {
    const std::vector<Id> order = walk();
    std::vector<Id> index(nodes_.size(), none);
    ByteWriter out;
    out.write(format);
    out.write(levels_);
    out.write_varint(order.size());
    for (std::size_t n = 0; n < order.size(); ++n) {
        const Node& node = nodes_[order[n]];
        index[order[n]] = static_cast<Id>(n);
        out.write(static_cast<std::uint8_t>((node.split() ? split_flag : 0U) |
                                            (node.open ? open_flag : 0U)));
        out.write(node.value);
        if (node.split()) {
            for (const Id child : node.children) {
                out.write_varint(index[child]);
            }
        }
    }
    return std::move(out).take();
}

template <typename Kind>
Result<Tree<Kind>> Tree<Kind>::decode(std::span<const std::byte> bytes) {
    ByteReader rest{bytes};
    const auto version = rest.read<std::uint8_t>();
    const auto levels = rest.read<std::uint8_t>();
    const auto count = rest.read_varint();
    if (!version || !levels || !count) {
        return make_error(ErrorCode::Parse, "a tree cut short");
    }
    if (*version != format) {
        return make_error(ErrorCode::Unsupported, std::format("tree format {}", *version));
    }
    if (*count == 0 || *count > rest.remaining()) {
        return make_error(ErrorCode::Parse, "a tree's node count past its bytes");
    }
    Tree tree;
    tree.levels_ = *levels;
    std::vector<Decoded> decoded;
    decoded.reserve(*count);
    for (std::uint64_t n = 0; n < *count; ++n) {
        auto node = tree.decode_node(rest, decoded);
        if (!node) {
            return std::unexpected(std::move(node.error()));
        }
        decoded.push_back(*node);
    }
    if (auto done = rest.finish(); !done) {
        return std::unexpected(std::move(done.error()));
    }
    tree.root_ = decoded.back().id;
    return tree;
}

template <typename Kind>
auto Tree<Kind>::decode_node(ByteReader& in, std::span<const Decoded> decoded) -> Result<Decoded> {
    const auto flags = in.read<std::uint8_t>();
    const auto value = in.read<Value>();
    if (!flags || !value) {
        return make_error(ErrorCode::Parse, "a tree node cut short");
    }
    if ((*flags & ~(split_flag | open_flag)) != 0) {
        return make_error(ErrorCode::Parse, std::format("tree node flags {}", *flags));
    }
    const bool split = (*flags & split_flag) != 0;
    if (split && (*flags & open_flag) != 0) {
        return make_error(ErrorCode::Parse, "a split tree node marked open");
    }
    Node node{.value = *value, .open = (*flags & open_flag) != 0};
    std::uint8_t reach = 0;
    if (split) {
        std::array<Value, 8> values{};
        for (std::size_t k = 0; k < 8; ++k) {
            const auto child = in.read_varint();
            if (!child || *child >= decoded.size()) {
                return make_error(ErrorCode::Parse, "a tree node's child not before it");
            }
            const Decoded& below = decoded[*child];
            node.children.at(k) = below.id;
            values.at(k) = nodes_[below.id].value;
            reach = std::max(reach, static_cast<std::uint8_t>(below.reach + 1));
        }
        if (reach > levels_) {
            return make_error(ErrorCode::Parse, "a tree deeper than its levels");
        }
        if (node.value != Kind::summary(values)) {
            return make_error(ErrorCode::Parse, "a split node's value isn't its summary");
        }
    }
    return Decoded{.id = held(node), .reach = reach};
}

template class Tree<Shape>;
template class Tree<Material>;
template class Tree<Water>;

} // namespace hog::grid
