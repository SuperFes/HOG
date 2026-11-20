#include <hog/server/chunk_sending.hpp>

#include <algorithm>
#include <format>
#include <type_traits>
#include <utility>

namespace hog::server {
namespace {

// As the protocol names the kinds.
template <typename Kind>
constexpr std::uint8_t kind_of() noexcept {
    if constexpr (std::is_same_v<Kind, grid::Shape>) {
        return 0;
    } else if constexpr (std::is_same_v<Kind, grid::Material>) {
        return 1;
    } else if constexpr (std::is_same_v<Kind, grid::Water>) {
        return 2;
    } else {
        static_assert(std::is_same_v<Kind, grid::Woods>);
        return 3;
    }
}

// How much of the deepest depth's space the node takes.
std::uint64_t volume(grid::Key key) noexcept {
    return std::uint64_t{1} << (3U * (grid::max_depth - key.depth));
}

// Whether a band written under `root` may have changed anything of `node` down to `depth`.
bool overlaps(const baked::Bands& bands, const baked::Region& written, grid::Key node,
              std::uint8_t depth) noexcept {
    if (!written.root.contains(node) && !node.contains(written.root)) {
        return false;
    }
    const auto from =
        static_cast<std::uint8_t>(written.band == 0 ? 0 : bands.roots(written.band) + 1);
    return std::max({from, written.root.depth, node.depth}) <=
           std::min(bands.foot(written.band), depth);
}

} // namespace

void ChunkSending::join(net::ConnectionId player) {
    players_.try_emplace(player);
}

void ChunkSending::leave(net::ConnectionId player) {
    players_.erase(player);
}

std::optional<ChunkSending::Open> ChunkSending::take_open(Player& player, grid::Key node) {
    for (std::deque<Open>* queue : {&player.again, &player.due}) {
        const auto found =
            std::ranges::find(*queue, node, [](const Open& open) { return open.want.node; });
        if (found != queue->end()) {
            Open open = *found;
            queue->erase(found);
            return open;
        }
    }
    if (const auto found = player.waiting.find(node); found != player.waiting.end()) {
        Open open = found->second;
        player.waiting.erase(found);
        return open;
    }
    return std::nullopt;
}

void ChunkSending::close(Player& player, std::uint64_t ask) {
    const auto found = player.asks.find(ask);
    if (found != player.asks.end() && --found->second.open == 0) {
        player.asks.erase(found);
    }
}

void ChunkSending::ask(net::ConnectionId player, std::span<const Want> wants) {
    Player& held = players_[player];
    held.round = ++rounds_;
    const std::uint64_t ask = ++held.asked;
    const std::uint8_t finest = bands_.foot(static_cast<std::uint8_t>(bands_.count() - 1));
    for (Want want : wants) {
        if (!want.node.is_valid() || want.node.depth > finest) {
            continue;
        }
        want.depth = std::clamp(want.depth, want.node.depth, finest);
        Open open{.want = want, .ask = ask};
        // In place of one asked of the node, what it was sent goes on counting as sent.
        if (const std::optional<Open> was = take_open(held, want.node)) {
            open.ask = was->ask;
            open.answered = was->answered && was->want.depth >= want.depth;
        } else {
            ++held.asks[ask].open;
        }
        held.due.push_back(open);
    }
}

void ChunkSending::drop(net::ConnectionId player, std::span<const grid::Key> nodes) {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return;
    }
    for (const grid::Key node : nodes) {
        if (const std::optional<Open> was = take_open(found->second, node)) {
            close(found->second, was->ask);
        }
    }
}

void ChunkSending::check(net::ConnectionId player, std::span<const Kept> kept) {
    Player& held = players_[player];
    held.checks.insert(held.checks.end(), kept.begin(), kept.end());
}

template <typename Kind>
bool ChunkSending::check_kept(const baked::ChunkStore::Snapshot& snapshot, const Kept& kept,
                              Limits& limits, Next& next) {
    const std::uint64_t now = snapshot.version<Kind>(kept.band, kept.root);
    if (now == kept.version) {
        return true;
    }
    protocol::SentChunk chunk{.kind = kind_of<Kind>(),
                              .band = kept.band,
                              .depth = kept.root.depth,
                              .x = kept.root.x,
                              .y = kept.root.y,
                              .z = kept.root.z,
                              .version = now,
                              .tree = {}};
    if (now != 0) {
        for (const baked::StoredChunk& each : snapshot.stored<Kind>(kept.band, kept.root)) {
            if (each.chunk.root == kept.root) {
                chunk.tree.assign(each.tree.begin(), each.tree.end());
            }
        }
        if (chunk.tree.size() > protocol::most_chunk) {
            ++next.too_big;
            return true;
        }
    }
    if (!limits.fits(next, chunk.tree.size())) {
        limits.cut = true;
        return false;
    }
    limits.bytes += chunk.tree.size();
    next.chunks.push_back(std::move(chunk));
    return true;
}

void ChunkSending::changed(std::span<const baked::Region> written) {
    for (auto& [id, player] : players_) {
        for (const baked::Region& region : written) {
            std::vector<Open> woken;
            grid::for_nested(player.waiting, region.root, [&](grid::Key node, const Open& open) {
                if (overlaps(bands_, region, node, open.want.depth)) {
                    woken.push_back(open);
                }
            });
            for (const Open& open : woken) {
                player.waiting.erase(open.want.node);
                player.again.push_back(open);
            }
        }
    }
}

void ChunkSending::changed_everything() {
    for (auto& [id, player] : players_) {
        for (auto& [ask, asked] : player.asks) {
            asked.sent.clear();
        }
        for (const auto& [node, open] : player.waiting) {
            player.again.push_back(open);
        }
        player.waiting.clear();
    }
}

std::vector<baked::WantsTable::Want> ChunkSending::wanted() const {
    std::map<std::pair<grid::Key, std::uint8_t>, baked::Rank, std::less<>> ranked;
    for (const auto& [id, player] : players_) {
        for (const auto& [node, open] : player.waiting) {
            if (open.answered) {
                continue;
            }
            const baked::Rank rank{.round = player.round, .priority = open.want.priority};
            auto [at, fresh] = ranked.try_emplace({node, open.want.depth}, rank);
            if (!fresh) {
                at->second = std::max(at->second, rank);
            }
        }
    }
    std::vector<baked::WantsTable::Want> wants;
    wants.reserve(ranked.size());
    for (const auto& [asked, rank] : ranked) {
        wants.push_back({.node = asked.first, .depth = asked.second, .rank = rank});
    }
    return wants;
}

void ChunkSending::answered(std::span<const baked::ChunkFiller::Asked> asked) {
    for (auto& [id, player] : players_) {
        for (const baked::ChunkFiller::Asked& ask : asked) {
            const auto found = player.waiting.find(ask.node);
            if (found == player.waiting.end() || found->second.want.depth > ask.depth) {
                continue;
            }
            Open open = found->second;
            player.waiting.erase(found);
            open.answered = true;
            player.again.push_back(open);
        }
    }
}

template <typename Kind>
bool ChunkSending::send_band(Sent& sent, const baked::ChunkStore::Snapshot& snapshot,
                             std::uint8_t band, grid::Key node, Limits& limits, Next& next) {
    constexpr std::uint8_t kind = kind_of<Kind>();
    bool over = false;
    std::uint64_t under = 0;
    for (const baked::StoredChunk& each : snapshot.stored<Kind>(band, node)) {
        const grid::Key root = each.chunk.root;
        const SentKey key{
            .kind = kind, .band = band, .code = grid::aligned(root), .depth = root.depth};
        if (root.contains(node)) {
            over = true;
        } else {
            under += volume(root);
        }
        const auto at = sent.find(key);
        if (at != sent.end() && at->second == each.chunk.version) {
            continue;
        }
        if (each.tree.size() > protocol::most_chunk) {
            sent.insert_or_assign(key, each.chunk.version);
            ++next.too_big;
            continue;
        }
        if (!limits.fits(next, each.tree.size())) {
            limits.cut = true;
            return false;
        }
        sent.insert_or_assign(key, each.chunk.version);
        next.chunks.push_back({.kind = kind,
                               .band = band,
                               .depth = root.depth,
                               .x = root.x,
                               .y = root.y,
                               .z = root.z,
                               .version = each.chunk.version,
                               .tree = {each.tree.begin(), each.tree.end()}});
        limits.bytes += each.tree.size();
    }
    return over || under == volume(node);
}

ChunkSending::Over ChunkSending::go_over(Player& player,
                                         const baked::ChunkStore::Snapshot& snapshot, Open& open,
                                         Limits& limits, Next& next) const {
    const Want& want = open.want;
    Sent& sent = player.asks[open.ask].sent;
    bool whole = true;
    for (std::uint8_t band = bands_.band_of(want.node.depth);
         band <= bands_.band_of(want.depth) && !limits.cut; ++band) {
        // Held all through the band already, the player needs none of it.
        if (bands_.foot(band) <= want.held) {
            continue;
        }
        whole = send_band<grid::Shape>(sent, snapshot, band, want.node, limits, next) && whole;
        if (!limits.cut) {
            send_band<grid::Material>(sent, snapshot, band, want.node, limits, next);
        }
        if (!limits.cut) {
            send_band<grid::Water>(sent, snapshot, band, want.node, limits, next);
        }
        if (!limits.cut) {
            send_band<grid::Woods>(sent, snapshot, band, want.node, limits, next);
        }
    }
    if (limits.cut) {
        return Over::Cut;
    }
    const protocol::DonePiece done{.depth = want.node.depth,
                                   .x = want.node.x,
                                   .y = want.node.y,
                                   .z = want.node.z,
                                   .to = want.depth,
                                   .whole = true};
    if (whole || open.answered) {
        next.done.push_back(done);
        return Over::Done;
    }
    if (!open.told) {
        next.done.push_back({.depth = done.depth,
                             .x = done.x,
                             .y = done.y,
                             .z = done.z,
                             .to = done.to,
                             .whole = false});
        open.told = true;
    }
    return Over::Waiting;
}

ChunkSending::Next ChunkSending::next(net::ConnectionId player,
                                      const baked::ChunkStore::Snapshot& snapshot, std::size_t room,
                                      std::size_t most) {
    Next next;
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return next;
    }
    Player& held = found->second;
    Limits limits{.room = room, .most = most};
    while (!held.checks.empty() && limits.fits(next, 0)) {
        const Kept& kept = held.checks.front();
        bool fit = true;
        switch (kept.kind) {
        case 0:
            fit = check_kept<grid::Shape>(snapshot, kept, limits, next);
            break;
        case 1:
            fit = check_kept<grid::Material>(snapshot, kept, limits, next);
            break;
        case 2:
            fit = check_kept<grid::Water>(snapshot, kept, limits, next);
            break;
        default:
            fit = check_kept<grid::Woods>(snapshot, kept, limits, next);
            break;
        }
        if (!fit) {
            return next;
        }
        held.checks.pop_front();
    }
    // Cut off, it's gone over again next, past what's gone already.
    for (std::deque<Open>* queue : {&held.again, &held.due}) {
        while (!queue->empty() && limits.fits(next, 0) && next.done.size() < protocol::most_asked) {
            Open& open = queue->front();
            const Over over = go_over(held, snapshot, open, limits, next);
            if (over == Over::Cut) {
                return next;
            }
            if (over == Over::Done) {
                close(held, open.ask);
            } else {
                held.waiting.insert_or_assign(open.want.node, open);
            }
            queue->pop_front();
        }
    }
    return next;
}

std::size_t ChunkSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0
                                   : found->second.checks.size() + found->second.again.size() +
                                         found->second.due.size();
}

std::size_t ChunkSending::waiting(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.waiting.size();
}

std::string ChunkSending::describe(net::ConnectionId player,
                                   const baked::ChunkStore::Snapshot& snapshot) const {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return "not a player\n";
    }
    const Player& held = found->second;
    // Each band the want reaches, as the store holds its shape, material and water there.
    const auto bands = [&](const Want& want) {
        std::string text;
        for (std::uint8_t band = bands_.band_of(want.node.depth);
             band <= bands_.band_of(want.depth); ++band) {
            text += std::format(" b{}:{}{}{}", band,
                                snapshot.holds<grid::Shape>(band, want.node) ? 's' : '-',
                                snapshot.holds<grid::Material>(band, want.node) ? 'm' : '-',
                                snapshot.holds<grid::Water>(band, want.node) ? 'w' : '-');
        }
        return text;
    };
    const auto line = [&](const Open& open) {
        const Want& want = open.want;
        return std::format("{} {} {} {} to {} held {} rank {:.6g} ask {}{} |{}\n", want.node.depth,
                           want.node.x, want.node.y, want.node.z, want.depth, want.held,
                           want.priority, open.ask, open.answered ? " answered" : "", bands(want));
    };
    std::size_t sent = 0;
    for (const auto& [ask, asked] : held.asks) {
        sent += asked.sent.size();
    }
    std::string text = std::format("round {}; {} asks, {} open, {} chunks sent for them\n",
                                   held.round, held.asked, held.asks.size(), sent);
    text += std::format("\nagain ({}):\n", held.again.size());
    for (const Open& open : held.again) {
        text += line(open);
    }
    text += std::format("\ndue ({}):\n", held.due.size());
    for (const Open& open : held.due) {
        text += line(open);
    }
    text += std::format("\nwaiting ({}):\n", held.waiting.size());
    for (const auto& [node, open] : held.waiting) {
        text += line(open);
    }
    return text;
}

std::vector<std::vector<std::byte>> bodies_coded(const grid::WaterBodies& table,
                                                 std::span<const grid::Water::Value> ids,
                                                 std::size_t most) {
    // The format byte and the count, a varint of ten bytes at most.
    constexpr std::size_t heading = 11;
    std::vector<std::vector<std::byte>> coded;
    grid::WaterBodies some;
    std::size_t bytes = heading;
    for (const grid::Water::Value id : ids) {
        const auto found = table.bodies.find(id);
        if (found == table.bodies.end()) {
            continue;
        }
        // Its own bytes, less the format byte and the one-byte count it's coded with alone.
        const std::size_t own = grid::WaterBodies{.bodies = {*found}}.encode().size() - 2;
        if (heading + own > most) {
            continue;
        }
        if (bytes + own > most) {
            coded.push_back(some.encode());
            some.bodies.clear();
            bytes = heading;
        }
        some.bodies.insert(*found);
        bytes += own;
    }
    if (!some.bodies.empty()) {
        coded.push_back(some.encode());
    }
    return coded;
}

} // namespace hog::server
