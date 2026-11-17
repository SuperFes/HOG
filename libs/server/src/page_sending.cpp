#include <hog/grid/leaves.hpp>
#include <hog/server/page_sending.hpp>

#include <algorithm>
#include <span>
#include <tuple>
#include <utility>

namespace hog::server {

PageSending::PageSending(const grid::Grid& grid, const grid::View& view)
    : grid_{grid}, view_{view} {}

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

std::vector<PageSending::Want> PageSending::wanted(DVec3 at, PageSource& source) const {
    std::vector<std::tuple<std::uint8_t, double, grid::Key, std::uint8_t>> found;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key page = pending.back();
        pending.pop_back();
        const double away = grid_.distance(page, at);
        const std::uint8_t levels = view_.levels_at(grid_, page, away);
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
            view_.under(grid_, leaf, foot, open.value.is_solid(), at, pending);
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
        if (sent == held.sent.end() || sent->second.levels < want.levels) {
            held.due.push_back(want);
        }
    }
}

void PageSending::changed(std::span<const grid::Key> pages) {
    for (auto& [id, held] : players_) {
        for (const grid::Key page : pages) {
            const auto sent = held.sent.find(page);
            if (sent == held.sent.end() || std::ranges::any_of(held.due, [&](const Want& want) {
                    return want.page == page;
                })) {
                continue;
            }
            held.due.insert(held.due.begin(), {.page = page, .levels = sent->second.levels});
        }
    }
}

namespace {

// Drops the trees of `page` no newer than those held, giving whether any is left.
bool keep_newer(grid::Page& page, std::uint32_t shape, std::uint32_t material,
                std::uint32_t water) {
    const auto keep = [](auto& tree, std::uint32_t held) {
        if (tree && tree->version <= held) {
            tree.reset();
        }
        return tree.has_value();
    };
    const bool any = keep(page.shape, shape);
    return keep(page.material, material) || keep(page.water, water) || any;
}

} // namespace

void PageSending::hold(Holding& holding, const grid::Page& page, std::uint8_t levels) {
    holding.levels = levels;
    holding.shape = page.shape ? page.shape->version : holding.shape;
    holding.material = page.material ? page.material->version : holding.material;
    holding.water = page.water ? page.water->version : holding.water;
}

bool PageSending::worth_sending(grid::Page& page, const Holding& holding) {
    // As deep as it's held, only what's changed goes; deeper, all of it does.
    const std::uint8_t levels = page.shape.value().tree.levels();
    if (levels > holding.levels) {
        return true;
    }
    return levels == holding.levels &&
           keep_newer(page, holding.shape, holding.material, holding.water);
}

std::vector<PageSending::Sent> PageSending::next(net::ConnectionId player, std::size_t room,
                                                 PageSource& source, std::size_t most) {
    std::vector<Sent> sent;
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return sent;
    }
    Player& held = found->second;
    std::size_t bytes = 0;
    std::vector<Want> still;
    for (const Want& want : held.due) {
        const bool open = sent.size() < most && (sent.empty() || bytes < room);
        std::optional<grid::Page> page = open ? source.page(want.page, want.levels) : std::nullopt;
        if (!page || !page->shape) {
            still.push_back(want);
            continue;
        }
        const auto had = held.sent.find(want.page);
        const std::uint8_t levels = page->shape.value().tree.levels();
        if (levels < want.levels) {
            still.push_back(want);
        }
        if (!worth_sending(*page, had == held.sent.end() ? Holding{} : had->second)) {
            continue;
        }
        std::vector<std::byte> coded = page.value().encode();
        if (!sent.empty() && bytes + coded.size() > room) {
            bytes = room;
            if (levels >= want.levels) {
                still.push_back(want);
            }
            continue;
        }
        bytes += coded.size();
        hold(held.sent[want.page], *page, levels);
        sent.push_back({.root = want.page, .bytes = std::move(coded)});
    }
    held.due = std::move(still);
    return sent;
}

std::size_t PageSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

} // namespace hog::server
