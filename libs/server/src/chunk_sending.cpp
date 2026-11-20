#include <hog/server/chunk_sending.hpp>

#include <algorithm>
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

void ChunkSending::ask(net::ConnectionId player, std::span<const Want> wants) {
    Player& held = players_[player];
    held.round = ++rounds_;
    const std::uint8_t finest = bands_.foot(static_cast<std::uint8_t>(bands_.count() - 1));
    std::vector<Want> asked;
    asked.reserve(wants.size());
    grid::ZMap<Want> waiting;
    grid::ZMap<Want> answered;
    for (Want want : wants) {
        if (!want.node.is_valid() || want.node.depth > finest) {
            continue;
        }
        want.depth = std::clamp(want.depth, want.node.depth, finest);
        // Still asked, what it waits on and what's known filled stay so till it's gone over.
        for (const auto& [from, into] :
             {std::pair{&held.waiting, &waiting}, std::pair{&held.answered, &answered}}) {
            if (const auto found = from->find(want.node);
                found != from->end() && found->second.depth >= want.depth) {
                into->insert(*found);
            }
        }
        asked.push_back(want);
    }
    held.asked = std::move(asked);
    held.gone = 0;
    held.again.clear();
    held.sent.clear();
    held.waiting = std::move(waiting);
    held.answered = std::move(answered);
}

void ChunkSending::changed(std::span<const baked::Region> written) {
    for (auto& [id, player] : players_) {
        for (const baked::Region& region : written) {
            std::vector<Want> woken;
            grid::for_nested(player.waiting, region.root, [&](grid::Key node, const Want& want) {
                if (overlaps(bands_, region, node, want.depth)) {
                    woken.push_back(want);
                }
            });
            for (const Want& want : woken) {
                player.waiting.erase(want.node);
                player.again.push_back(want);
            }
        }
    }
}

void ChunkSending::changed_everything() {
    for (auto& [id, player] : players_) {
        player.gone = 0;
        player.again.clear();
        player.sent.clear();
    }
}

std::vector<baked::WantsTable::Want> ChunkSending::wanted() const {
    std::map<std::pair<grid::Key, std::uint8_t>, baked::Rank, std::less<>> ranked;
    for (const auto& [id, player] : players_) {
        for (const auto& [node, want] : player.waiting) {
            const baked::Rank rank{.round = player.round, .priority = want.priority};
            auto [at, fresh] = ranked.try_emplace({node, want.depth}, rank);
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
            if (found == player.waiting.end() || found->second.depth > ask.depth) {
                continue;
            }
            const Want want = found->second;
            player.waiting.erase(found);
            player.answered.insert_or_assign(want.node, want);
            player.again.push_back(want);
        }
    }
}

template <typename Kind>
bool ChunkSending::send_band(Player& player, const baked::ChunkStore::Snapshot& snapshot,
                             std::uint8_t band, grid::Key node, Limits& limits, Next& next) const {
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
        const auto at = player.sent.find(key);
        if (at != player.sent.end() && at->second == each.chunk.version) {
            continue;
        }
        if (each.tree.size() > protocol::most_chunk) {
            player.sent.insert_or_assign(key, each.chunk.version);
            ++next.too_big;
            continue;
        }
        if (!limits.fits(next, each.tree.size())) {
            limits.cut = true;
            return false;
        }
        player.sent.insert_or_assign(key, each.chunk.version);
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

bool ChunkSending::go_over(Player& player, const baked::ChunkStore::Snapshot& snapshot,
                           const Want& want, Limits& limits, Next& next) const {
    bool whole = true;
    for (std::uint8_t band = bands_.band_of(want.node.depth);
         band <= bands_.band_of(want.depth) && !limits.cut; ++band) {
        whole = send_band<grid::Shape>(player, snapshot, band, want.node, limits, next) && whole;
        if (!limits.cut) {
            send_band<grid::Material>(player, snapshot, band, want.node, limits, next);
        }
        if (!limits.cut) {
            send_band<grid::Water>(player, snapshot, band, want.node, limits, next);
        }
        if (!limits.cut) {
            send_band<grid::Woods>(player, snapshot, band, want.node, limits, next);
        }
    }
    if (limits.cut) {
        return true;
    }
    const auto told = player.answered.find(want.node);
    if (whole || (told != player.answered.end() && told->second.depth >= want.depth)) {
        player.waiting.erase(want.node);
    } else {
        player.waiting.insert_or_assign(want.node, want);
    }
    return false;
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
    // Cut off, it's gone over again next, past what's gone already.
    while (!held.again.empty() && limits.fits(next, 0)) {
        if (go_over(held, snapshot, held.again.front(), limits, next)) {
            return next;
        }
        held.again.erase(held.again.begin());
    }
    while (held.gone < held.asked.size() && limits.fits(next, 0)) {
        if (go_over(held, snapshot, held.asked[held.gone], limits, next)) {
            return next;
        }
        ++held.gone;
    }
    return next;
}

std::size_t ChunkSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end()
               ? 0
               : found->second.again.size() + (found->second.asked.size() - found->second.gone);
}

std::size_t ChunkSending::waiting(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.waiting.size();
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
