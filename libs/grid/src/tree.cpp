#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace hog::grid {

namespace {

constexpr std::uint8_t split_flag = 1;
constexpr std::uint8_t open_flag = 2;

} // namespace

template <typename Kind>
Tree<Kind>::Tree(std::uint8_t levels, Value whole, bool open)
    : root_(leaf_of(whole, open)), levels_(levels) {}

template <typename Kind>
std::size_t Tree<Kind>::Hash::operator()(const Node& node) const noexcept {
    std::uint64_t hash = hash_values(0, Kind::hash(node.value), node.open);
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
auto Tree<Kind>::joined(const std::array<Id, 8>& children, double side,
                        const std::optional<Value>& own) -> Id {
    std::array<Value, 8> values{};
    for (std::size_t k = 0; k < 8; ++k) {
        values.at(k) = nodes_[children.at(k)].value;
    }
    const Node& first = nodes_[children[0]];
    if (std::ranges::none_of(children, [&](Id child) {
            const Node& node = nodes_[child];
            return node.split() || node.open != first.open;
        })) {
        if (const std::optional<Value> one = Kind::merged(values, side)) {
            return leaf_of(own.value_or(*one), first.open);
        }
    }
    return held(
        {.value = own.value_or(Kind::summary(values)), .open = false, .children = children});
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

namespace {

// Which of a node's eight children holds (x, y, z) among the depth `shift` levels below them.
std::size_t octant_of(std::uint32_t x, std::uint32_t y, std::uint32_t z, unsigned shift) noexcept {
    return ((x >> shift) & 1U) | (((y >> shift) & 1U) << 1U) | (((z >> shift) & 1U) << 2U);
}

} // namespace

template <typename Kind>
Tree<Kind> Tree<Kind>::cut(std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                           std::uint8_t levels) const {
    Id id = root_;
    for (std::uint8_t down = 0; down < depth; ++down) {
        const Node& node = nodes_[id];
        if (!node.split()) {
            return Tree{levels, node.value, node.open};
        }
        id = node.children.at(octant_of(x, y, z, static_cast<unsigned>(depth - down - 1)));
    }
    Tree piece;
    piece.levels_ = levels;
    // Shared nodes are copied once for each depth they're met at.
    std::unordered_map<std::uint64_t, Id> copied;
    const auto key = [](Id from, std::uint8_t below) {
        return (std::uint64_t{from} << 8U) | below;
    };
    // A node `below` levels under the piece's root as it's copied, where that needs no children
    // copied first.
    const auto copied_now = [&](Id from, std::uint8_t below) -> std::optional<Id> {
        const Node& node = nodes_[from];
        if (!node.split()) {
            return piece.leaf_of(node.value, node.open);
        }
        if (below == levels) {
            return piece.leaf_of(node.value, true);
        }
        if (const auto found = copied.find(key(from, below)); found != copied.end()) {
            return found->second;
        }
        return std::nullopt;
    };
    struct Frame {
        Id from = 0;
        std::uint8_t below = 0;
        std::size_t next = 0;
        std::array<Id, 8> children{};
    };
    if (const std::optional<Id> now = copied_now(id, 0)) {
        piece.root_ = *now;
        return piece;
    }
    std::vector<Frame> stack{{.from = id, .below = 0, .next = 0, .children = {}}};
    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next == 8) {
            const Id made = piece.held(
                {.value = nodes_[top.from].value, .open = false, .children = top.children});
            copied.emplace(key(top.from, top.below), made);
            stack.pop_back();
            if (stack.empty()) {
                piece.root_ = made;
                break;
            }
            Frame& parent = stack.back();
            parent.children.at(parent.next++) = made;
            continue;
        }
        const Id child = nodes_[top.from].children.at(top.next);
        const auto below = static_cast<std::uint8_t>(top.below + 1);
        if (const std::optional<Id> now = copied_now(child, below)) {
            top.children.at(top.next++) = *now;
        } else {
            stack.push_back({.from = child, .below = below, .next = 0, .children = {}});
        }
    }
    return piece;
}

template <typename Kind>
void Tree<Kind>::graft(std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z,
                       const Tree& piece) {
    // The piece's nodes held here, children first.
    std::vector<Id> mapped(piece.nodes_.size(), none);
    for (const Id from : piece.walk()) {
        Node node = piece.nodes_[from];
        if (node.split()) {
            for (Id& child : node.children) {
                child = mapped[child];
            }
        }
        mapped[from] = held(node);
    }
    // The way down from the root; a leaf on it stands for each of its children below.
    std::vector<Id> way(depth);
    Id at = root_;
    for (std::uint8_t down = 0; down < depth; ++down) {
        way[down] = at;
        if (const Node& node = nodes_[at]; node.split()) {
            at = node.children.at(octant_of(x, y, z, static_cast<unsigned>(depth - down - 1)));
        }
    }
    Id id = mapped[piece.root_];
    for (std::uint8_t down = depth; down-- > 0;) {
        const Node over = nodes_[way[down]];
        std::array<Id, 8> children{};
        if (over.split()) {
            children = over.children;
        } else {
            children.fill(way[down]);
        }
        children.at(octant_of(x, y, z, static_cast<unsigned>(depth - down - 1))) = id;
        id = held({.value = over.value, .open = false, .children = children});
    }
    root_ = id;
    levels_ = std::max(levels_, static_cast<std::uint8_t>(depth + piece.levels_));
}

template <typename Kind>
void Tree<Kind>::compact() {
    Tree kept;
    kept.levels_ = levels_;
    std::vector<Id> mapped(nodes_.size(), none);
    for (const Id from : walk()) {
        Node node = nodes_[from];
        if (node.split()) {
            for (Id& child : node.children) {
                child = mapped[child];
            }
        }
        mapped[from] = kept.held(node);
    }
    kept.root_ = mapped[root_];
    *this = std::move(kept);
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
auto Tree<Kind>::leaf_values() const -> std::vector<Value> {
    std::vector<Value> values;
    for (const Id id : walk()) {
        if (!nodes_[id].split()) {
            values.push_back(nodes_[id].value);
        }
    }
    return values;
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
        Kind::write(out, node.value);
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
    const auto value = Kind::read(in);
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
        for (std::size_t k = 0; k < 8; ++k) {
            const auto child = in.read_varint();
            if (!child || *child >= decoded.size()) {
                return make_error(ErrorCode::Parse, "a tree node's child not before it");
            }
            const Decoded& below = decoded[*child];
            node.children.at(k) = below.id;
            reach = std::max(reach, static_cast<std::uint8_t>(below.reach + 1));
        }
        if (reach > levels_) {
            return make_error(ErrorCode::Parse, "a tree deeper than its levels");
        }
    }
    return Decoded{.id = held(node), .reach = reach};
}

template class Tree<Shape>;
template class Tree<Material>;
template class Tree<Water>;
template class Tree<Woods>;

} // namespace hog::grid
