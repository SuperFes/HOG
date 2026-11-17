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

} // namespace

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
        depth.emplace(root, Held{.page = std::move(*page), .bytes = bytes.size()});
        ++count_;
        bytes_ += bytes.size();
        return {};
    }
    Held& held = found->second;
    if (levels_of(*page) > levels_of(held.page)) {
        held.page = std::move(*page);
    } else {
        held.page.generator = page->generator;
        const auto replace = [](auto& tree, auto& sent) {
            if (sent) {
                tree = std::move(sent);
            }
        };
        replace(held.page.shape, page->shape);
        replace(held.page.material, page->material);
        replace(held.page.water, page->water);
    }
    bytes_ -= held.bytes;
    held.bytes = held.page.encode().size();
    bytes_ += held.bytes;
    return {};
}

Result<void> HeldPages::take_bodies(std::span<const std::byte> bytes) {
    auto sent = grid::WaterBodies::decode(bytes);
    if (!sent) {
        return std::unexpected{sent.error()};
    }
    for (auto& [id, body] : sent->bodies) {
        bodies_.bodies.insert_or_assign(id, std::move(body));
    }
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
    return held == nullptr ? nullptr : &held->page;
}

template <typename Kind>
std::optional<HeldPages::Known<Kind>> HeldPages::at(grid::Key node) const {
    if (!node.is_valid() || node.depth > grid_.depth()) {
        return std::nullopt;
    }
    // Finest first: the page under the node where it's a page's root itself.
    grid::Key root =
        node.depth < grid_.depth() ? grid_.page_of(node.child(0)) : grid_.page_of(node);
    while (true) {
        const Held* held = find(root);
        if (const auto* tree = held == nullptr ? nullptr : &tree_of<Kind>(held->page);
            tree != nullptr && tree->has_value()) {
            const auto below = static_cast<std::uint8_t>(
                std::min<int>(node.depth - root.depth, tree->value().tree.levels()));
            const grid::Key over = node.ancestor(static_cast<std::uint8_t>(root.depth + below));
            auto found =
                tree->value().tree.at(below, over.x - (root.x << below), over.y - (root.y << below),
                                      over.z - (root.z << below));
            found.depth = static_cast<std::uint8_t>(found.depth + root.depth);
            return Known<Kind>{.found = found, .page = root};
        }
        if (root.depth == 0) {
            return std::nullopt;
        }
        root = grid_.page_of(root);
    }
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
