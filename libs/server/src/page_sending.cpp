#include <hog/grid/leaves.hpp>
#include <hog/server/page_sending.hpp>

#include <algorithm>
#include <functional>
#include <span>
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

std::vector<PageSending::Want> PageSending::wanted(DVec3 at, PageSource& source,
                                                   std::uint64_t round) const {
    std::vector<Want> found;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key page = pending.back();
        pending.pop_back();
        const double away = grid_.distance(page, at);
        const std::uint8_t levels = view_.levels_at(grid_, page, away);
        const double priority = grid_.size(page.depth) / (away + grid_.finest());
        found.push_back({.page = page, .levels = levels, .priority = priority});
        const std::uint8_t whole = grid_.levels_under(page.depth);
        const auto foot = static_cast<std::uint8_t>(page.depth + whole);
        if (levels < whole || foot >= grid_.depth()) {
            continue;
        }
        const std::optional<grid::Tree<grid::Shape>> shape =
            source.shape(page, levels, {.round = round, .priority = priority});
        if (!shape || shape->levels() < whole) {
            continue;
        }
        grid::for_open_leaves(*shape, page, [&](grid::Key leaf, const auto& open) {
            view_.under(grid_, leaf, foot, open.value, at, pending);
        });
    }
    std::ranges::sort(found, [](const Want& a, const Want& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.page < b.page;
    });
    return found;
}

void PageSending::follow(net::ConnectionId player, DVec3 at, PageSource& source) {
    Player& held = players_[player];
    held.over = over(at);
    held.round = ++rounds_;
    held.due.clear();
    held.waiting.clear();
    for (const Want& want : wanted(at, source, held.round)) {
        const auto sent = held.sent.find(want.page);
        if (sent == held.sent.end() || sent->second.levels < want.levels) {
            held.due.push_back(want);
        }
    }
}

void PageSending::ask(net::ConnectionId player, std::span<const Want> wants) {
    Player& held = players_[player];
    held.asking = true;
    held.round = ++rounds_;
    held.due.clear();
    held.waiting.clear();
    for (Want want : wants) {
        const grid::Key page = want.page;
        if (page.depth >= grid_.depth()) {
            continue;
        }
        const std::uint32_t across = std::uint32_t{1} << page.depth;
        if (page.x >= across || page.y >= across || page.z >= across ||
            grid_.page_of(page.child(0)) != page) {
            continue;
        }
        want.levels = std::min(want.levels, grid_.levels_under(page.depth));
        const auto sent = held.sent.find(page);
        if (want.levels > 0 && (sent == held.sent.end() || sent->second.levels < want.levels)) {
            held.due.push_back(want);
        }
    }
}

bool PageSending::asking(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found != players_.end() && found->second.asking;
}

void PageSending::changed(std::span<const grid::Key> pages) {
    for (auto& [id, held] : players_) {
        for (const grid::Key page : pages) {
            if (const auto waits = held.waiting.find(page); waits != held.waiting.end()) {
                const Want want = waits->second;
                held.waiting.erase(waits);
                held.due.insert(std::ranges::upper_bound(held.due, want.priority,
                                                         std::ranges::greater{}, &Want::priority),
                                want);
                continue;
            }
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
template <typename Holding>
bool keep_newer(grid::Page& page, const Holding& held) {
    const auto keep = [](auto& tree, std::uint32_t version) {
        if (tree && tree->version <= version) {
            tree.reset();
        }
        return tree.has_value();
    };
    const bool shape = keep(page.shape, held.shape);
    const bool material = keep(page.material, held.material);
    const bool water = keep(page.water, held.water);
    const bool woods = keep(page.woods, held.woods);
    return shape || material || water || woods;
}

} // namespace

void PageSending::hold(Holding& holding, const grid::Page& page, std::uint8_t levels) {
    holding.levels = levels;
    holding.shape = page.shape ? page.shape->version : holding.shape;
    holding.material = page.material ? page.material->version : holding.material;
    holding.water = page.water ? page.water->version : holding.water;
    holding.woods = page.woods ? page.woods->version : holding.woods;
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
        if (sent.size() >= most || (!sent.empty() && bytes >= room)) {
            still.push_back(want);
            continue;
        }
        const std::shared_ptr<const Answer> answer =
            source.answer(want.page, want.levels, {.round = held.round, .priority = want.priority});
        if (!answer) {
            held.waiting[want.page] = want;
            continue;
        }
        const auto had = held.sent.find(want.page);
        const Holding holding = had == held.sent.end() ? Holding{} : had->second;
        const std::uint8_t levels = answer->levels();
        if (levels < want.levels) {
            held.waiting[want.page] = want;
        }
        // Deeper than held, the answer goes as it's kept; as deep, only what's newer of it.
        std::vector<std::byte> coded;
        if (levels > holding.levels) {
            coded = answer->bytes;
        } else if (levels == holding.levels) {
            grid::Page newer = answer->page;
            if (!keep_newer(newer, holding)) {
                continue;
            }
            coded = newer.encode();
        } else {
            continue;
        }
        if (!sent.empty() && bytes + coded.size() > room) {
            bytes = room;
            held.waiting.erase(want.page);
            still.push_back(want);
            continue;
        }
        bytes += coded.size();
        hold(held.sent[want.page], answer->page, levels);
        sent.push_back({.root = want.page,
                        .bytes = std::move(coded),
                        .bodies = newly_told(held.told, answer->bodies)});
    }
    held.due = std::move(still);
    return sent;
}

std::vector<grid::Water::Value>
PageSending::newly_told(std::set<grid::Water::Value>& told,
                        std::span<const grid::Water::Value> marked) {
    std::vector<grid::Water::Value> bodies;
    for (const grid::Water::Value body : marked) {
        if (body != 0 && told.insert(body).second) {
            bodies.push_back(body);
        }
    }
    return bodies;
}

std::size_t PageSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

std::size_t PageSending::waiting(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.waiting.size();
}

} // namespace hog::server
