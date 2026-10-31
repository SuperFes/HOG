#include <hog/core/assert.hpp>
#include <hog/globe/hexel_store.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace hog::globe {
namespace {

constexpr std::uint32_t brick_side = 8;
constexpr int brick_span = 8;
constexpr int roots = 12;

} // namespace

HexelStore::HexelStore(int level, Shells shells, Hexel fill) : level_(level), shells_(shells) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    HOG_ASSERT(shells.count > 0, "a store needs shells");
    nodes_.assign(roots, Node{.hexel = fill});
}

HexelStore::Box HexelStore::root(int diamond) const noexcept {
    const bool pole = std::cmp_greater_equal(diamond, HexCell::north_pole);
    return {.side = pole ? 1U : std::uint32_t{1} << level_,
            .span = static_cast<int>(std::bit_ceil(static_cast<unsigned>(shells_.count)))};
}

namespace {

template <typename Box>
Box half(const Box& box, bool across, int k) {
    Box child = box;
    if (across) {
        child.side = box.side / 2;
        child.i += static_cast<std::uint32_t>(k & 1) * child.side;
        child.j += static_cast<std::uint32_t>((k >> 1) & 1) * child.side;
    } else {
        child.span = box.span / 2;
        child.shell += k * child.span;
    }
    return child;
}

template <typename Box>
int half_holding(const Box& box, bool across, std::uint32_t i, std::uint32_t j, int shell) {
    if (across) {
        const std::uint32_t mid = box.side / 2;
        return (i - box.i >= mid ? 1 : 0) + (j - box.j >= mid ? 2 : 0);
    }
    return shell - box.shell >= box.span / 2 ? 1 : 0;
}

template <typename Box>
std::size_t brick_index(const Box& box, std::uint32_t i, std::uint32_t j, int shell) {
    return (((static_cast<std::size_t>(shell - box.shell) * box.side) + (j - box.j)) * box.side) +
           (i - box.i);
}

template <typename Box>
std::size_t brick_size(const Box& box) {
    return static_cast<std::size_t>(box.side) * box.side * static_cast<std::size_t>(box.span);
}

} // namespace

Hexel HexelStore::at(HexCell cell, int shell) const {
    HOG_ASSERT(std::cmp_equal(cell.level, level_) && cell.is_valid(), "cell not at this level");
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    Box box = root(cell.diamond);
    std::uint32_t at = cell.diamond;
    for (;;) {
        const Node& node = nodes_[at];
        switch (node.kind) {
        case Kind::Whole:
            return node.hexel;
        case Kind::Brick:
            return bricks_[node.at + brick_index(box, cell.i, cell.j, shell)];
        case Kind::Across:
        case Kind::Up: {
            const bool across = node.kind == Kind::Across;
            const int k = half_holding(box, across, cell.i, cell.j, shell);
            box = half(box, across, k);
            at = node.at + static_cast<std::uint32_t>(k);
        }
        }
    }
}

void HexelStore::set(HexCell cell, int shell, Hexel hexel) {
    HOG_ASSERT(std::cmp_equal(cell.level, level_) && cell.is_valid(), "cell not at this level");
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    paint(cell.diamond, root(cell.diamond),
          {.i0 = cell.i,
           .i1 = cell.i + 1,
           .j0 = cell.j,
           .j1 = cell.j + 1,
           .shell0 = shell,
           .shell1 = shell + 1},
          hexel);
}

void HexelStore::fill(int from, int to, Hexel hexel) {
    HOG_ASSERT(from >= 0 && from <= to && to <= shells_.count, "shells out of range");
    // The padding above the top shell goes with it, so the top can be whole.
    const Region region{.i1 = std::numeric_limits<std::uint32_t>::max(),
                        .j1 = std::numeric_limits<std::uint32_t>::max(),
                        .shell0 = from,
                        .shell1 = to == shells_.count ? std::numeric_limits<int>::max() : to};
    for (int diamond = 0; diamond < roots; ++diamond) {
        paint(static_cast<std::uint32_t>(diamond), root(diamond), region, hexel);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tree, a few dozen at most
void HexelStore::paint(std::uint32_t node, const Box& box, const Region& region, Hexel hexel) {
    const std::uint32_t i1 = box.i + box.side;
    const std::uint32_t j1 = box.j + box.side;
    const int shell1 = box.shell + box.span;
    if (region.i0 >= i1 || region.i1 <= box.i || region.j0 >= j1 || region.j1 <= box.j ||
        region.shell0 >= shell1 || region.shell1 <= box.shell) {
        return;
    }
    const bool across_whole =
        region.i0 <= box.i && i1 <= region.i1 && region.j0 <= box.j && j1 <= region.j1;
    const bool up_whole = region.shell0 <= box.shell && shell1 <= region.shell1;
    if (across_whole && up_whole) {
        nodes_[node] = {.hexel = hexel};
        return;
    }
    const Node here = nodes_[node];
    if (here.kind == Kind::Whole && here.hexel == hexel) {
        return;
    }
    if (here.kind == Kind::Brick || (box.side <= brick_side && box.span <= brick_span)) {
        paint_brick(node, box, region, hexel);
        return;
    }
    Kind kind = here.kind;
    if (kind == Kind::Whole) {
        // Split only the way the region cuts, so a layer stays whole across the globe.
        if (across_whole) {
            kind = Kind::Up;
        } else if (up_whole) {
            kind = Kind::Across;
        } else {
            kind = box.side > brick_side ? Kind::Across : Kind::Up;
        }
        const auto first = static_cast<std::uint32_t>(nodes_.size());
        nodes_.insert(nodes_.end(), kind == Kind::Across ? 4 : 2, Node{.hexel = here.hexel});
        nodes_[node] = {.hexel = {}, .kind = kind, .at = first};
    }
    const bool across = kind == Kind::Across;
    const std::uint32_t first = nodes_[node].at;
    for (int k = 0; k < (across ? 4 : 2); ++k) {
        paint(first + static_cast<std::uint32_t>(k), half(box, across, k), region, hexel);
    }
}

void HexelStore::paint_brick(std::uint32_t node, const Box& box, const Region& region,
                             Hexel hexel) {
    if (nodes_[node].kind != Kind::Brick) {
        const auto first = static_cast<std::uint32_t>(bricks_.size());
        bricks_.insert(bricks_.end(), brick_size(box), nodes_[node].hexel);
        nodes_[node] = {.hexel = {}, .kind = Kind::Brick, .at = first};
    }
    const std::uint32_t first = nodes_[node].at;
    const int shell1 = std::min(box.shell + box.span, region.shell1);
    const std::uint32_t j1 = std::min(box.j + box.side, region.j1);
    const std::uint32_t i1 = std::min(box.i + box.side, region.i1);
    for (int shell = std::max(box.shell, region.shell0); shell < shell1; ++shell) {
        for (std::uint32_t j = std::max(box.j, region.j0); j < j1; ++j) {
            for (std::uint32_t i = std::max(box.i, region.i0); i < i1; ++i) {
                bricks_[first + brick_index(box, i, j, shell)] = hexel;
            }
        }
    }
}

void HexelStore::compact() {
    std::vector<Node> nodes(roots);
    std::vector<Hexel> bricks;
    for (int diamond = 0; diamond < roots; ++diamond) {
        const Node made =
            compacted(static_cast<std::uint32_t>(diamond), root(diamond), nodes, bricks);
        nodes[static_cast<std::size_t>(diamond)] = made;
    }
    nodes_ = std::move(nodes);
    bricks_ = std::move(bricks);
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tree, a few dozen at most
HexelStore::Node HexelStore::compacted(std::uint32_t node, const Box& box, std::vector<Node>& into,
                                       std::vector<Hexel>& bricks) const {
    const Node here = nodes_[node];
    if (here.kind == Kind::Whole) {
        return here;
    }
    if (here.kind == Kind::Brick) {
        const auto begin = std::next(bricks_.begin(), static_cast<std::ptrdiff_t>(here.at));
        // Only shells below the top count; the padding above may hold anything.
        const auto in_range = static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(box.side) * box.side *
            static_cast<std::size_t>(std::clamp(shells_.count - box.shell, 1, box.span)));
        if (std::all_of(begin, std::next(begin, in_range),
                        [&](const Hexel& hexel) { return hexel == *begin; })) {
            return {.hexel = *begin};
        }
        const auto first = static_cast<std::uint32_t>(bricks.size());
        bricks.insert(bricks.end(), begin,
                      std::next(begin, static_cast<std::ptrdiff_t>(brick_size(box))));
        return {.hexel = {}, .kind = Kind::Brick, .at = first};
    }
    const bool across = here.kind == Kind::Across;
    const int count = across ? 4 : 2;
    const std::size_t first = into.size();
    into.resize(first + static_cast<std::size_t>(count));
    bool one = true;
    for (int k = 0; k < count; ++k) {
        const Box child = half(box, across, k);
        const Node made = compacted(here.at + static_cast<std::uint32_t>(k), child, into, bricks);
        into[first + static_cast<std::size_t>(k)] = made;
        if (child.shell < shells_.count) {
            one = one && made.kind == Kind::Whole && made.hexel == into[first].hexel;
        }
    }
    if (one) {
        // Whole children made nothing further, so they're the last in.
        const Hexel hexel = into[first].hexel;
        into.resize(first);
        return {.hexel = hexel};
    }
    return {.hexel = {}, .kind = here.kind, .at = static_cast<std::uint32_t>(first)};
}

std::size_t HexelStore::bytes() const noexcept {
    return (nodes_.size() * sizeof(Node)) + (bricks_.size() * sizeof(Hexel));
}

HexelStore pack(const Globe& globe) {
    HOG_ASSERT(globe.is_whole(), "only a whole globe packs");
    HexelStore store{globe.level(), globe.shells()};
    for (std::uint64_t index = 0; index < cell_count(globe.level()); ++index) {
        const HexCell cell = cell_at_index(globe.level(), index);
        const auto column = globe.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            if (column[shell] != Hexel{}) {
                store.set(cell, static_cast<int>(shell), column[shell]);
            }
        }
    }
    store.compact();
    return store;
}

void unpack(const HexelStore& store, Globe& globe) {
    HOG_ASSERT(globe.is_whole() && store.level() == globe.level() &&
                   store.shells().count == globe.shells().count,
               "store and globe differ in shape");
    for (std::uint64_t index = 0; index < cell_count(globe.level()); ++index) {
        const HexCell cell = cell_at_index(globe.level(), index);
        const auto column = globe.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            column[shell] = store.at(cell, static_cast<int>(shell));
        }
    }
}

Globe window(const HexelStore& store, std::vector<HexCell> cells, int from, int to) {
    HOG_ASSERT(0 <= from && from < to && to <= store.shells().count, "shells out of range");
    Globe globe{store.level(), store.shells().part(from, to), std::move(cells), from};
    for (std::uint64_t index = 0; index < globe.size(); ++index) {
        const HexCell cell = globe.cell(index);
        const auto column = globe.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            column[shell] = store.at(cell, from + static_cast<int>(shell));
        }
    }
    return globe;
}

void put_back(const Globe& window, HexelStore& store) {
    HOG_ASSERT(window.level() == store.level() &&
                   window.first_shell() + window.shells().count <= store.shells().count,
               "the window is not onto this store");
    for (std::uint64_t index = 0; index < window.size(); ++index) {
        const HexCell cell = window.cell(index);
        const auto column = window.column_at(index);
        for (std::size_t shell = 0; shell < column.size(); ++shell) {
            store.set(cell, window.first_shell() + static_cast<int>(shell), column[shell]);
        }
    }
    store.compact();
}

} // namespace hog::globe
