#include <hog/client/page_asking.hpp>
#include <hog/grid/leaves.hpp>

#include <algorithm>

namespace hog::client {

std::vector<PageWish> pages_to_ask(const HeldPages& held, DVec3 eye, std::optional<DVec3> facing,
                                   const grid::View& view, std::size_t most) {
    const grid::Grid& grid = held.grid();
    std::vector<PageWish> wishes;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key page = pending.back();
        pending.pop_back();
        const double away = grid.distance(page, eye);
        const std::uint8_t levels = view.levels_at(grid, page, away);
        const grid::Page* has = held.page(page);
        const std::uint8_t holds = has != nullptr && has->shape ? has->shape->tree.levels() : 0;
        if (holds < levels) {
            double priority = grid.size(page.depth) / (away + grid.finest());
            // Wholly behind where the eye looks: its nearest point is behind the eye.
            if (facing && away > 0.0) {
                const DVec3 low = grid.low(page);
                const DVec3 nearest = glm::clamp(eye, low, low + DVec3{grid.size(page.depth)});
                if (glm::dot(nearest - eye, *facing) < 0.0) {
                    priority *= behind_the_eye;
                }
            }
            wishes.push_back({.page = page, .levels = levels, .priority = priority});
        }
        const std::uint8_t whole = grid.levels_under(page.depth);
        const auto foot = static_cast<std::uint8_t>(page.depth + whole);
        if (levels < whole || holds < whole || foot >= grid.depth() || has == nullptr ||
            !has->shape) {
            continue;
        }
        grid::for_open_leaves(has->shape->tree, page, [&](grid::Key leaf, const auto& open) {
            view.under(grid, leaf, foot, open.value, eye, pending);
        });
    }
    std::ranges::sort(wishes, [](const PageWish& a, const PageWish& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.page < b.page;
    });
    if (wishes.size() > most) {
        wishes.resize(most);
    }
    return wishes;
}

protocol::Ask ask_of(const std::vector<PageWish>& wishes) {
    protocol::Ask ask;
    ask.pages.reserve(std::min(wishes.size(), protocol::most_asked));
    for (const PageWish& wish : wishes) {
        if (ask.pages.size() == protocol::most_asked) {
            break;
        }
        ask.pages.push_back({.depth = wish.page.depth,
                             .x = wish.page.x,
                             .y = wish.page.y,
                             .z = wish.page.z,
                             .levels = wish.levels,
                             .priority = static_cast<float>(wish.priority)});
    }
    return ask;
}

} // namespace hog::client
