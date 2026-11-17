#include <hog/client/drawn_pages.hpp>
#include <hog/grid/leaves.hpp>

#include <algorithm>

namespace hog::client {

std::vector<DrawnPage> drawn_pages(const HeldPages& held, DVec3 eye, const grid::View& view) {
    const grid::Grid& grid = held.grid();
    const auto drawable = [&](grid::Key root) {
        const grid::Page* page = held.page(root);
        return page != nullptr && page->shape ? &page->shape->tree : nullptr;
    };
    std::vector<DrawnPage> drawn;
    std::vector<grid::Key> pending;
    if (drawable(grid::Key{}) != nullptr) {
        pending.push_back(grid::Key{});
    }
    while (!pending.empty()) {
        const grid::Key root = pending.back();
        pending.pop_back();
        const grid::Tree<grid::Shape>* tree = drawable(root);
        if (tree == nullptr) {
            continue;
        }
        const std::uint8_t whole = grid.levels_under(root.depth);
        const auto foot = static_cast<std::uint8_t>(root.depth + whole);
        DrawnPage page{.page = root,
                       .levels = std::min(view.levels_at(grid, root, grid.distance(root, eye)),
                                          tree->levels()),
                       .under = {}};
        if (page.levels == whole && foot < grid.depth()) {
            std::vector<grid::Key> feet;
            grid::for_open_leaves(*tree, root, [&](grid::Key leaf, const auto& open) {
                view.under(grid, leaf, foot, open.value.is_solid(), eye, feet);
            });
            for (const grid::Key under : feet) {
                if (drawable(under) != nullptr) {
                    page.under.push_back(under);
                    pending.push_back(under);
                }
            }
            std::ranges::sort(page.under);
        }
        drawn.push_back(std::move(page));
    }
    std::ranges::sort(drawn, {}, &DrawnPage::page);
    return drawn;
}

} // namespace hog::client
