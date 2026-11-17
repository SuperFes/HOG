#include <hog/grid/leaves.hpp>
#include <hog/server/page_sending.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace hog::server {

PageSending::PageSending(const grid::Grid& grid, double pixels)
    : grid_{grid}, angle_{pixels * view_fov / view_height} {}

void PageSending::join(net::ConnectionId player) {
    players_.try_emplace(player);
}

void PageSending::leave(net::ConnectionId player) {
    players_.erase(player);
}

grid::Key PageSending::over(DVec3 at) const {
    const std::optional<grid::Key> finest = grid_.key_at(at, grid_.depth());
    return finest ? grid_.page_of(*finest) : grid::Key{};
}

bool PageSending::moved(net::ConnectionId player, DVec3 at) const {
    const auto found = players_.find(player);
    return found == players_.end() || found->second.over != over(at);
}

bool PageSending::refined(std::uint8_t foot, double metres) const noexcept {
    return grid_.size(foot) > angle_ * metres;
}

std::uint8_t PageSending::levels_at(grid::Key root, double metres) const noexcept {
    const std::uint8_t whole = grid_.levels_under(root.depth);
    for (std::uint8_t levels = 1; levels < whole; ++levels) {
        if (grid_.size(static_cast<std::uint8_t>(root.depth + levels)) <= angle_ * metres) {
            return levels;
        }
    }
    return whole;
}

std::vector<PageSending::Want> PageSending::wanted(DVec3 at, PageSource& source) const {
    // Ground open for caves shows only through openings in it, so it's followed only where the
    // finest nodes are wanted.
    const double cave_reach = grid_.finest() / angle_;
    std::vector<std::tuple<std::uint8_t, double, grid::Key, std::uint8_t>> found;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key page = pending.back();
        pending.pop_back();
        const double away = grid_.distance(page, at);
        const std::uint8_t levels = levels_at(page, away);
        found.emplace_back(page.depth, away, page, levels);
        const std::uint8_t whole = grid_.levels_under(page.depth);
        const auto foot = static_cast<std::uint8_t>(page.depth + whole);
        if (levels < whole || foot >= grid_.depth()) {
            continue;
        }
        const std::optional<grid::Tree<grid::Shape>> shape = source.shape(page, levels);
        if (!shape || shape->levels() < whole) {
            continue;
        }
        grid::for_open_leaves(*shape, page, [&](grid::Key leaf, const auto& open) {
            const bool surface = !open.value.is_solid();
            std::vector<grid::Key> under{leaf};
            while (!under.empty()) {
                const grid::Key key = under.back();
                under.pop_back();
                // Nothing inside is nearer, so nothing inside is wanted either.
                const double metres = grid_.distance(key, at);
                if (!refined(foot, metres) || (!surface && metres > cave_reach)) {
                    continue;
                }
                if (key.depth == foot) {
                    pending.push_back(key);
                    continue;
                }
                for (std::size_t octant = 0; octant < 8; ++octant) {
                    under.push_back(key.child(octant));
                }
            }
        });
    }
    std::ranges::sort(found);
    std::vector<Want> wants;
    wants.reserve(found.size());
    for (const auto& [depth, away, page, levels] : found) {
        wants.push_back({.page = page, .levels = levels});
    }
    return wants;
}

void PageSending::follow(net::ConnectionId player, DVec3 at, PageSource& source) {
    Player& held = players_[player];
    held.over = over(at);
    held.due.clear();
    for (const Want& want : wanted(at, source)) {
        const auto sent = held.sent.find(want.page);
        if (sent == held.sent.end() || sent->second < want.levels) {
            held.due.push_back(want);
        }
    }
}

std::vector<PageSending::Sent> PageSending::next(net::ConnectionId player, std::size_t room,
                                                 PageSource& source) {
    std::vector<Sent> sent;
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return sent;
    }
    Player& held = found->second;
    std::size_t bytes = 0;
    std::vector<Want> still;
    for (const Want& want : held.due) {
        const bool full = !sent.empty() && bytes >= room;
        std::optional<grid::Page> page = full ? std::nullopt : source.page(want.page, want.levels);
        const std::uint8_t had = held.sent.contains(want.page) ? held.sent[want.page] : 0;
        const std::uint8_t levels =
            page && page->shape ? page->shape.value().tree.levels() : std::uint8_t{0};
        if (!page || levels <= had) {
            still.push_back(want);
            continue;
        }
        std::vector<std::byte> coded = page.value().encode();
        if (!sent.empty() && bytes + coded.size() > room) {
            bytes = room;
            still.push_back(want);
            continue;
        }
        bytes += coded.size();
        held.sent[want.page] = levels;
        sent.push_back({.root = want.page, .bytes = std::move(coded)});
        if (levels < want.levels) {
            still.push_back(want);
        }
    }
    held.due = std::move(still);
    return sent;
}

std::size_t PageSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

} // namespace hog::server
