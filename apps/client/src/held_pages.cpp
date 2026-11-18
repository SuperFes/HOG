#include <hog/client/held_pages.hpp>

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>

namespace hog::client {
namespace {

template <typename Kind>
const std::optional<grid::Held<Kind>>& tree_of(const grid::Page& page) {
    if constexpr (std::is_same_v<Kind, grid::Shape>) {
        return page.shape;
    } else if constexpr (std::is_same_v<Kind, grid::Material>) {
        return page.material;
    } else {
        return page.water;
    }
}

// How deep a page's trees go; none where it carries none.
int levels_of(const grid::Page& page) {
    if (page.shape) {
        return page.shape->tree.levels();
    }
    if (page.material) {
        return page.material->tree.levels();
    }
    return page.water ? page.water->tree.levels() : -1;
}

// What's known at `node` among the pages `page_at` finds by their roots, as HeldPages::at has it.
template <typename Kind, typename PageAt>
std::optional<KnownAt<Kind>> known_at(const grid::Grid& grid, grid::Key node,
                                      const PageAt& page_at) {
    if (!node.is_valid() || node.depth > grid.depth()) {
        return std::nullopt;
    }
    // Finest first: the page under the node where it's a page's root itself.
    grid::Key root = node.depth < grid.depth() ? grid.page_of(node.child(0)) : grid.page_of(node);
    while (true) {
        const grid::Page* page = page_at(root);
        if (const auto* tree = page == nullptr ? nullptr : &tree_of<Kind>(*page);
            tree != nullptr && tree->has_value()) {
            const auto below = static_cast<std::uint8_t>(
                std::min<int>(node.depth - root.depth, tree->value().tree.levels()));
            const grid::Key over = node.ancestor(static_cast<std::uint8_t>(root.depth + below));
            auto found =
                tree->value().tree.at(below, over.x - (root.x << below), over.y - (root.y << below),
                                      over.z - (root.z << below));
            found.depth = static_cast<std::uint8_t>(found.depth + root.depth);
            return KnownAt<Kind>{.found = found, .page = root};
        }
        if (root.depth == 0) {
            return std::nullopt;
        }
        root = grid.page_of(root);
    }
}

} // namespace

PagesSnapshot::PagesSnapshot(const grid::Grid& grid, std::vector<Entry> pages)
    : grid_{grid}, pages_{std::move(pages)} {
    std::ranges::sort(pages_, {}, &Entry::first);
}

const PagesSnapshot::Entry* PagesSnapshot::find(grid::Key root) const {
    const auto found = std::ranges::lower_bound(pages_, root, {}, &Entry::first);
    return found == pages_.end() || found->first != root ? nullptr : &*found;
}

const grid::Page* PagesSnapshot::page(grid::Key root) const {
    const Entry* entry = find(root);
    return entry == nullptr ? nullptr : entry->second.get();
}

std::shared_ptr<const grid::Page> PagesSnapshot::shared(grid::Key root) const {
    const Entry* entry = find(root);
    return entry == nullptr ? nullptr : entry->second;
}

template <typename Kind>
std::optional<KnownAt<Kind>> PagesSnapshot::at(grid::Key node) const {
    return known_at<Kind>(grid_, node, [this](grid::Key root) { return page(root); });
}

template std::optional<KnownAt<grid::Shape>> PagesSnapshot::at<grid::Shape>(grid::Key) const;
template std::optional<KnownAt<grid::Water>> PagesSnapshot::at<grid::Water>(grid::Key) const;

std::vector<PagesSnapshot::Entry> PagesSnapshot::around(grid::Key node) const {
    std::vector<Entry> found;
    over_around(node, found);
    finer_around(node, found);
    std::ranges::sort(found, {}, &Entry::first);
    const auto [from, to] = std::ranges::unique(found, {}, &Entry::first);
    found.erase(from, to);
    return found;
}

void PagesSnapshot::over_around(grid::Key node, std::vector<Entry>& found) const {
    const auto add = [&](grid::Key root) {
        if (const Entry* entry = find(root)) {
            found.push_back(*entry);
        }
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto next = node.beside(dx, dy, dz);
                if (!next) {
                    continue;
                }
                grid::Key root = next->depth < grid_.depth() ? grid_.page_of(next->child(0))
                                                             : grid_.page_of(*next);
                for (; root.depth > 0; root = grid_.page_of(root)) {
                    add(root);
                }
                add(root);
            }
        }
    }
}

void PagesSnapshot::finer_around(grid::Key node, std::vector<Entry>& found) const {
    for (auto depth = static_cast<std::uint8_t>(node.depth + 1); depth < grid_.depth(); ++depth) {
        const auto shift = static_cast<unsigned>(depth - node.depth);
        const auto low = [&](std::uint32_t at) {
            return (at << shift) == 0 ? 0 : (at << shift) - 1;
        };
        const auto high = [&](std::uint32_t at) { return (at + 1) << shift; };
        const auto within = [&](grid::Key root) {
            return root.y >= low(node.y) && root.y <= high(node.y) && root.z >= low(node.z) &&
                   root.z <= high(node.z);
        };
        const grid::Key first{.depth = depth, .x = low(node.x), .y = low(node.y), .z = low(node.z)};
        for (auto at = std::ranges::lower_bound(pages_, first, {}, &Entry::first);
             at != pages_.end() && at->first.depth == depth && at->first.x <= high(node.x); ++at) {
            if (within(at->first)) {
                found.push_back(*at);
            }
        }
    }
}

HeldPages::HeldPages(const grid::Grid& grid)
    : grid_{grid}, held_(static_cast<std::size_t>(grid.depth()) + 1) {}

Result<void> HeldPages::take(std::span<const std::byte> bytes) {
    auto page = grid::Page::decode(bytes);
    if (!page) {
        return std::unexpected{page.error()};
    }
    const grid::Key root = page->root;
    if (!root.is_valid() || root.depth >= grid_.depth() || grid_.page_of(root.child(0)) != root) {
        return make_error(ErrorCode::InvalidArgument, "a page rooted off the grid's pages");
    }
    if (levels_of(*page) > grid_.levels_under(root.depth)) {
        return make_error(ErrorCode::InvalidArgument, "a page deeper than its own");
    }
    Depth& depth = held_.at(root.depth);
    const auto found = depth.find(root);
    if (found == depth.end()) {
        depth.emplace(root, Held{.page = std::make_shared<const grid::Page>(std::move(*page)),
                                 .bytes = bytes.size()});
        ++count_;
        bytes_ += bytes.size();
        return {};
    }
    Held& held = found->second;
    // Held pages are shared with what reads them, so a page changed is a new one.
    if (levels_of(*page) <= levels_of(*held.page)) {
        grid::Page kept = *held.page;
        kept.generator = page->generator;
        const auto replace = [](auto& tree, auto& sent) {
            if (sent) {
                tree = std::move(sent);
            }
        };
        replace(kept.shape, page->shape);
        replace(kept.material, page->material);
        replace(kept.water, page->water);
        *page = std::move(kept);
    }
    held.page = std::make_shared<const grid::Page>(std::move(*page));
    bytes_ -= held.bytes;
    held.bytes = held.page->encode().size();
    bytes_ += held.bytes;
    return {};
}

Result<void> HeldPages::take_bodies(std::span<const std::byte> bytes) {
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

const HeldPages::Held* HeldPages::find(grid::Key root) const {
    if (root.depth >= held_.size()) {
        return nullptr;
    }
    const Depth& depth = held_[root.depth];
    const auto found = depth.find(root);
    return found == depth.end() ? nullptr : &found->second;
}

const grid::Page* HeldPages::page(grid::Key root) const {
    const Held* held = find(root);
    return held == nullptr ? nullptr : held->page.get();
}

std::shared_ptr<const grid::Page> HeldPages::shared(grid::Key root) const {
    const Held* held = find(root);
    return held == nullptr ? nullptr : held->page;
}

std::vector<PagesSnapshot::Entry> HeldPages::entries() const {
    std::vector<PagesSnapshot::Entry> entries;
    entries.reserve(count_);
    for (const Depth& depth : held_) {
        for (const auto& [root, held] : depth) {
            entries.emplace_back(root, held.page);
        }
    }
    return entries;
}

template <typename Kind>
std::optional<HeldPages::Known<Kind>> HeldPages::at(grid::Key node) const {
    return known_at<Kind>(grid_, node, [this](grid::Key root) {
        const Held* held = find(root);
        return held == nullptr ? nullptr : held->page.get();
    });
}

template std::optional<HeldPages::Known<grid::Shape>> HeldPages::at<grid::Shape>(grid::Key) const;
template std::optional<HeldPages::Known<grid::Material>>
    HeldPages::at<grid::Material>(grid::Key) const;
template std::optional<HeldPages::Known<grid::Water>> HeldPages::at<grid::Water>(grid::Key) const;

std::vector<grid::Key> HeldPages::evict(DVec3 eye, std::size_t budget) {
    std::vector<std::pair<double, grid::Key>> candidates;
    for (std::size_t depth = 1; depth < held_.size(); ++depth) {
        for (const auto& [root, held] : held_[depth]) {
            candidates.emplace_back(grid_.distance(root, eye), root);
        }
    }
    std::ranges::sort(candidates, std::greater{});
    std::vector<grid::Key> dropped;
    for (const auto& [metres, root] : candidates) {
        if (bytes_ <= budget) {
            break;
        }
        Depth& depth = held_[root.depth];
        const auto found = depth.find(root);
        if (found == depth.end()) {
            continue;
        }
        bytes_ -= found->second.bytes;
        --count_;
        depth.erase(found);
        dropped.push_back(root);
    }
    return dropped;
}

} // namespace hog::client
