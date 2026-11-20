#include <hog/client/held_pieces.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace hog::client {
namespace {

template <typename Kind>
std::optional<grid::Tree<Kind>>& tree_of(grid::Piece& piece) {
    if constexpr (std::is_same_v<Kind, grid::Shape>) {
        return piece.shape;
    } else if constexpr (std::is_same_v<Kind, grid::Material>) {
        return piece.material;
    } else if constexpr (std::is_same_v<Kind, grid::Water>) {
        return piece.water;
    } else {
        return piece.woods;
    }
}

template <typename Kind>
const std::optional<grid::Tree<Kind>>& tree_of(const grid::Piece& piece) {
    if constexpr (std::is_same_v<Kind, grid::Shape>) {
        return piece.shape;
    } else if constexpr (std::is_same_v<Kind, grid::Material>) {
        return piece.material;
    } else if constexpr (std::is_same_v<Kind, grid::Water>) {
        return piece.water;
    } else {
        return piece.woods;
    }
}

// How deep a piece's trees go; none where it carries none.
int levels_of(const grid::Piece& piece) {
    if (piece.shape) {
        return piece.shape->levels();
    }
    if (piece.material) {
        return piece.material->levels();
    }
    if (piece.water) {
        return piece.water->levels();
    }
    return piece.woods ? piece.woods->levels() : -1;
}

// What's known at `node` among the pieces `piece_at` finds by their roots, rooted at the depths
// `depths` marks, as HeldPieces::at has it. Deeper roots are tried first, and a shallower one
// only where it might reach deeper than what's found.
template <typename Kind, typename PieceAt>
std::optional<Known<Kind>> known_at(const grid::Grid& grid, std::uint64_t depths, grid::Key node,
                                    const PieceAt& piece_at) {
    if (!node.is_valid() || node.depth > grid.depth()) {
        return std::nullopt;
    }
    std::optional<Known<Kind>> best;
    for (int depth = node.depth; depth >= 0; --depth) {
        if (((depths >> static_cast<unsigned>(depth)) & 1U) == 0U) {
            continue;
        }
        const grid::Key root = node.ancestor(static_cast<std::uint8_t>(depth));
        const grid::Piece* piece = piece_at(root);
        if (piece == nullptr) {
            continue;
        }
        const std::optional<grid::Tree<Kind>>& held = tree_of<Kind>(*piece);
        if (!held) {
            continue;
        }
        const grid::Tree<Kind>& tree = *held;
        if (best && root.depth + tree.levels() <= best->found.depth) {
            continue;
        }
        const auto below =
            static_cast<std::uint8_t>(std::min<int>(node.depth - root.depth, tree.levels()));
        const grid::Key over = node.ancestor(static_cast<std::uint8_t>(root.depth + below));
        auto found = tree.at(below, over.x - (root.x << below), over.y - (root.y << below),
                             over.z - (root.z << below));
        found.depth = static_cast<std::uint8_t>(found.depth + root.depth);
        if (!best || found.depth > best->found.depth) {
            best = Known<Kind>{.found = found, .root = root};
        }
        // Nothing over it can say more than the node itself or a closed leaf.
        if (best->found.depth == node.depth || !best->found.open) {
            break;
        }
    }
    return best;
}

// Whether the place of `root`, deeper than `node`, lies in the node's or touches it.
bool touches(grid::Key node, grid::Key root) {
    const auto shift = static_cast<unsigned>(root.depth - node.depth);
    const auto near = [&](std::uint32_t at, std::uint32_t of) {
        const std::uint32_t low = of << shift;
        return at + 1 >= low && at <= ((of + 1) << shift);
    };
    return near(root.x, node.x) && near(root.y, node.y) && near(root.z, node.z);
}

std::uint64_t depth_bit(std::uint8_t depth) {
    return std::uint64_t{1} << depth;
}

} // namespace

PiecesSnapshot::PiecesSnapshot(const grid::Grid& grid, std::vector<Entry> pieces)
    : grid_{grid}, pieces_{std::move(pieces)} {
    std::ranges::sort(pieces_, grid::ZOrder{}, &Entry::first);
    index_.reserve(pieces_.size());
    for (std::size_t at = 0; at < pieces_.size(); ++at) {
        depths_ |= depth_bit(pieces_[at].first.depth);
        index_.emplace(grid::code(pieces_[at].first), at);
    }
}

const PiecesSnapshot::Entry* PiecesSnapshot::find(grid::Key root) const {
    const auto found = index_.find(grid::code(root));
    return found == index_.end() ? nullptr : &pieces_[found->second];
}

const grid::Piece* PiecesSnapshot::piece(grid::Key root) const {
    const Entry* entry = find(root);
    return entry == nullptr ? nullptr : entry->second.get();
}

template <typename Kind>
std::optional<Known<Kind>> PiecesSnapshot::at(grid::Key node) const {
    return known_at<Kind>(grid_, depths_, node, [this](grid::Key root) { return piece(root); });
}

template std::optional<Known<grid::Shape>> PiecesSnapshot::at<grid::Shape>(grid::Key) const;
template std::optional<Known<grid::Material>> PiecesSnapshot::at<grid::Material>(grid::Key) const;
template std::optional<Known<grid::Water>> PiecesSnapshot::at<grid::Water>(grid::Key) const;
template std::optional<Known<grid::Woods>> PiecesSnapshot::at<grid::Woods>(grid::Key) const;

void PiecesSnapshot::over(grid::Key node, std::vector<Entry>& found) const {
    for (std::uint8_t depth = 0; depth <= node.depth; ++depth) {
        if ((depths_ & depth_bit(depth)) == 0U) {
            continue;
        }
        if (const Entry* entry = find(node.ancestor(depth))) {
            found.push_back(*entry);
        }
    }
}

void PiecesSnapshot::under_touching(grid::Key node, grid::Key place,
                                    std::vector<Entry>& found) const {
    const std::uint64_t end = grid::aligned_end(place);
    for (auto at = std::ranges::upper_bound(pieces_, place, grid::ZOrder{}, &Entry::first);
         at != pieces_.end() && grid::aligned(at->first) <= end; ++at) {
        if (touches(node, at->first)) {
            found.push_back(*at);
        }
    }
}

std::vector<PiecesSnapshot::Entry> PiecesSnapshot::around(grid::Key node) const {
    std::vector<Entry> found;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (const std::optional<grid::Key> next = node.beside(dx, dy, dz)) {
                    over(*next, found);
                    under_touching(node, *next, found);
                }
            }
        }
    }
    std::ranges::sort(found, grid::ZOrder{}, &Entry::first);
    const auto [from, to] = std::ranges::unique(found, {}, &Entry::first);
    found.erase(from, to);
    return found;
}

HeldPieces::HeldPieces(const grid::Grid& grid) : grid_{grid} {}

Result<void> HeldPieces::take(const protocol::SentChunk& chunk) {
    const grid::Key root{.depth = chunk.depth, .x = chunk.x, .y = chunk.y, .z = chunk.z};
    if (!root.is_valid() || root.depth > grid_.depth()) {
        return make_error(ErrorCode::InvalidArgument, "a chunk rooted off the grid");
    }
    if (chunk.kind >= kinds) {
        return make_error(ErrorCode::InvalidArgument, "a chunk of no kind");
    }
    if (chunk.version == 0) {
        let_go(chunk.kind, chunk.band, root);
        return {};
    }
    // A band's chunks never nest, so one of its kind held over or under this one is out of date.
    std::vector<grid::Key> stale;
    for (std::uint8_t depth = 0; depth < root.depth; ++depth) {
        if (const auto over = held_.find(root.ancestor(depth));
            over != held_.end() && std::cmp_equal(over->second.bands.at(chunk.kind), chunk.band)) {
            stale.push_back(over->first);
        }
    }
    const std::uint64_t end = grid::aligned_end(root);
    for (auto under = held_.upper_bound(root);
         under != held_.end() && grid::aligned(under->first) <= end; ++under) {
        if (std::cmp_equal(under->second.bands.at(chunk.kind), chunk.band)) {
            stale.push_back(under->first);
        }
    }
    for (const grid::Key key : stale) {
        let_go(chunk.kind, chunk.band, key);
    }
    switch (chunk.kind) {
    case 0:
        return take_tree<grid::Shape>(chunk, root);
    case 1:
        return take_tree<grid::Material>(chunk, root);
    case 2:
        return take_tree<grid::Water>(chunk, root);
    default:
        return take_tree<grid::Woods>(chunk, root);
    }
}

template <typename Kind>
Result<void> HeldPieces::take_tree(const protocol::SentChunk& chunk, grid::Key root) {
    auto tree = grid::Tree<Kind>::decode(chunk.tree);
    if (!tree) {
        return std::unexpected{tree.error()};
    }
    if (root.depth + tree->levels() > grid_.depth()) {
        return make_error(ErrorCode::InvalidArgument, "a chunk deeper than the grid");
    }
    Held& held = held_[root];
    const std::size_t kind = chunk.kind;
    if (std::cmp_greater(held.bands.at(kind), chunk.band)) {
        return {};
    }
    // Held pieces are shared with what reads them, so a piece changed is a new one.
    grid::Piece piece = held.piece ? *held.piece : grid::Piece{};
    piece.root = root;
    tree_of<Kind>(piece) = std::move(*tree);
    held.piece = std::make_shared<const grid::Piece>(std::move(piece));
    held.bands.at(kind) = chunk.band;
    bytes_ += chunk.tree.size() - held.bytes.at(kind);
    held.bytes.at(kind) = chunk.tree.size();
    held.reach = static_cast<std::uint8_t>(root.depth + std::max(levels_of(*held.piece), 0));
    depths_ |= depth_bit(root.depth);
    return {};
}

void HeldPieces::let_go(std::size_t kind, int band, grid::Key root) {
    const auto found = held_.find(root);
    if (found == held_.end() || found->second.bands.at(kind) != band) {
        return;
    }
    Held& held = found->second;
    grid::Piece piece = *held.piece;
    switch (kind) {
    case 0:
        piece.shape.reset();
        break;
    case 1:
        piece.material.reset();
        break;
    case 2:
        piece.water.reset();
        break;
    default:
        piece.woods.reset();
        break;
    }
    bytes_ -= held.bytes.at(kind);
    held.bytes.at(kind) = 0;
    held.bands.at(kind) = -1;
    if (levels_of(piece) < 0) {
        held_.erase(found);
        return;
    }
    held.piece = std::make_shared<const grid::Piece>(std::move(piece));
    held.reach = static_cast<std::uint8_t>(root.depth + levels_of(*held.piece));
}

Result<void> HeldPieces::take_bodies(std::span<const std::byte> bytes) {
    auto sent = grid::WaterBodies::decode(bytes);
    if (!sent) {
        return std::unexpected{sent.error()};
    }
    auto bodies = std::make_shared<grid::WaterBodies>(*bodies_);
    for (auto& [id, body] : sent->bodies) {
        bodies->bodies.insert_or_assign(id, std::move(body));
    }
    bodies_ = std::move(bodies);
    return {};
}

const HeldPieces::Held* HeldPieces::find(grid::Key root) const {
    const auto found = held_.find(root);
    return found == held_.end() ? nullptr : &found->second;
}

const grid::Piece* HeldPieces::piece(grid::Key root) const {
    const Held* held = find(root);
    return held == nullptr ? nullptr : held->piece.get();
}

std::optional<std::uint8_t> HeldPieces::reach(grid::Key root) const {
    const Held* held = find(root);
    return held == nullptr ? std::nullopt : std::optional{held->reach};
}

template <typename Kind>
std::optional<Known<Kind>> HeldPieces::at(grid::Key node) const {
    return known_at<Kind>(grid_, depths_, node, [this](grid::Key root) { return piece(root); });
}

template std::optional<Known<grid::Shape>> HeldPieces::at<grid::Shape>(grid::Key) const;
template std::optional<Known<grid::Material>> HeldPieces::at<grid::Material>(grid::Key) const;
template std::optional<Known<grid::Water>> HeldPieces::at<grid::Water>(grid::Key) const;
template std::optional<Known<grid::Woods>> HeldPieces::at<grid::Woods>(grid::Key) const;

bool HeldPieces::covers(grid::Key node, std::uint8_t depth) const {
    if (!node.is_valid() || node.depth > grid_.depth()) {
        return false;
    }
    depth = std::min(std::max(depth, node.depth), grid_.depth());
    // Woods lie over the ground as an overlay of their own, so the ground doesn't wait on them.
    return covers_kind<grid::Shape>(node, depth) && covers_kind<grid::Material>(node, depth) &&
           covers_kind<grid::Water>(node, depth);
}

template <typename Kind>
bool HeldPieces::covers_kind(grid::Key node, std::uint8_t depth) const {
    std::vector<grid::Key> pending{node};
    while (!pending.empty()) {
        const grid::Key next = pending.back();
        pending.pop_back();
        if (held_over<Kind>(next, depth)) {
            continue;
        }
        // Only worth going down where pieces lie under the node.
        const auto under = held_.upper_bound(next);
        if (next.depth == depth || under == held_.end() ||
            grid::aligned(under->first) > grid::aligned_end(next)) {
            return false;
        }
        for (std::size_t octant = 0; octant < 8; ++octant) {
            pending.push_back(next.child(octant));
        }
    }
    return true;
}

template <typename Kind>
bool HeldPieces::held_over(grid::Key node, std::uint8_t depth) const {
    for (std::uint8_t over = 0; over <= node.depth; ++over) {
        if ((depths_ & depth_bit(over)) == 0U) {
            continue;
        }
        const grid::Key root = node.ancestor(over);
        if (const Held* held = find(root); held != nullptr) {
            const std::optional<grid::Tree<Kind>>& tree = tree_of<Kind>(*held->piece);
            if (tree && root.depth + tree->levels() >= depth) {
                return true;
            }
        }
    }
    // A node found as itself may split; one found over a child of it is a leaf. Where the
    // shape's leaf is closed nothing finer of any kind is made, though another kind's be open.
    const grid::Key probe = node.depth < grid_.depth() ? node.child(0) : node;
    const auto known = at<grid::Shape>(probe);
    return known && !known->found.open && known->found.depth <= node.depth;
}

std::vector<PiecesSnapshot::Entry> HeldPieces::entries() const {
    std::vector<PiecesSnapshot::Entry> entries;
    entries.reserve(held_.size());
    for (const auto& [root, held] : held_) {
        entries.emplace_back(root, held.piece);
    }
    return entries;
}

} // namespace hog::client
