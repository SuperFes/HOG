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

grid::Key root_of(std::uint64_t code, std::uint8_t depth) noexcept {
    const std::uint64_t bits = code >> (3U * (grid::max_depth - depth));
    return {.depth = depth,
            .x = grid::gather(bits),
            .y = grid::gather(bits >> 1U),
            .z = grid::gather(bits >> 2U)};
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
    held.due.clear();
    const std::uint8_t finest = bands_.foot(static_cast<std::uint8_t>(bands_.count() - 1));
    grid::ZMap<Want> waiting;
    for (Want want : wants) {
        if (!want.node.is_valid() || want.node.depth > finest) {
            continue;
        }
        want.depth = std::clamp(want.depth, want.node.depth, finest);
        if (const auto whole = held.whole.find(want.node);
            whole != held.whole.end() && whole->second.depth >= want.depth) {
            continue;
        }
        // Waiting on a fill already, it goes again once something's written under it.
        if (const auto still = held.waiting.find(want.node);
            still != held.waiting.end() && still->second.depth >= want.depth) {
            waiting.insert(*still);
            continue;
        }
        held.due.push_back(want);
    }
    held.waiting = std::move(waiting);
}

void ChunkSending::due_again(Player& player, const Want& want) {
    if (std::ranges::any_of(player.due, [&](const Want& due) { return due.node == want.node; })) {
        return;
    }
    player.due.insert(
        std::ranges::find_if(player.due,
                             [&](const Want& due) { return due.priority < want.priority; }),
        want);
}

void ChunkSending::changed(std::span<const baked::Region> written) {
    for (auto& [id, player] : players_) {
        for (const baked::Region& region : written) {
            for (grid::ZMap<Want>* held : {&player.waiting, &player.whole}) {
                std::vector<Want> woken;
                grid::for_nested(*held, region.root, [&](grid::Key node, const Want& want) {
                    if (overlaps(bands_, region, node, want.depth)) {
                        woken.push_back(want);
                    }
                });
                for (const Want& want : woken) {
                    held->erase(want.node);
                    due_again(player, want);
                }
            }
        }
    }
}

void ChunkSending::answered(std::span<const baked::ChunkFiller::Asked> asked) {
    for (auto& [id, player] : players_) {
        for (const baked::ChunkFiller::Asked& ask : asked) {
            const auto found = player.waiting.find(ask.node);
            if (found == player.waiting.end() || found->second.depth > ask.depth) {
                continue;
            }
            player.whole.insert_or_assign(ask.node, found->second);
            player.waiting.erase(found);
        }
    }
}

template <typename Kind>
bool ChunkSending::send_band(Player& player, const baked::ChunkStore::Snapshot& snapshot,
                             std::uint8_t band, grid::Key node, Limits& limits, Next& next) const {
    constexpr std::uint8_t kind = kind_of<Kind>();
    const std::vector<baked::StoredChunk> stored = snapshot.stored<Kind>(band, node);
    std::vector<SentKey> current;
    current.reserve(stored.size());
    bool over = false;
    std::uint64_t under = 0;
    for (const baked::StoredChunk& each : stored) {
        const grid::Key root = each.chunk.root;
        const SentKey key{
            .kind = kind, .band = band, .code = grid::aligned(root), .depth = root.depth};
        current.push_back(key);
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
    // Those sent about the node that the store no longer holds.
    std::vector<SentKey> gone;
    const auto held = [&](const SentKey& key) { return std::ranges::contains(current, key); };
    for (std::uint8_t depth = 0; depth <= node.depth; ++depth) {
        const grid::Key over_it = node.ancestor(depth);
        const SentKey key{
            .kind = kind, .band = band, .code = grid::aligned(over_it), .depth = depth};
        if (player.sent.contains(key) && !held(key)) {
            gone.push_back(key);
        }
    }
    const SentKey first{.kind = kind,
                        .band = band,
                        .code = grid::aligned(node),
                        .depth = static_cast<std::uint8_t>(node.depth + 1)};
    const std::uint64_t end = grid::aligned_end(node);
    for (auto at = player.sent.lower_bound(first);
         at != player.sent.end() && at->first.kind == kind && at->first.band == band &&
         at->first.code <= end;
         ++at) {
        if (!held(at->first)) {
            gone.push_back(at->first);
        }
    }
    for (const SentKey& key : gone) {
        if (!limits.fits(next, 0)) {
            limits.cut = true;
            return false;
        }
        player.sent.erase(key);
        // The player lets go of what it held at the root, so the other bands' go again.
        for (std::uint8_t other = 0; other < bands_.count(); ++other) {
            player.sent.erase({.kind = kind, .band = other, .code = key.code, .depth = key.depth});
        }
        const grid::Key root = root_of(key.code, key.depth);
        next.chunks.push_back({.kind = kind,
                               .band = band,
                               .depth = root.depth,
                               .x = root.x,
                               .y = root.y,
                               .z = root.z,
                               .version = 0,
                               .tree = {}});
    }
    return over || under == volume(node);
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
    std::size_t done = 0;
    for (; done < held.due.size() && limits.fits(next, 0); ++done) {
        const Want want = held.due[done];
        bool whole = true;
        for (std::uint8_t band = bands_.band_of(want.node.depth);
             band <= bands_.band_of(want.depth) && !limits.cut; ++band) {
            whole = send_band<grid::Shape>(held, snapshot, band, want.node, limits, next) && whole;
            if (!limits.cut) {
                send_band<grid::Material>(held, snapshot, band, want.node, limits, next);
            }
            if (!limits.cut) {
                send_band<grid::Water>(held, snapshot, band, want.node, limits, next);
            }
            if (!limits.cut) {
                send_band<grid::Woods>(held, snapshot, band, want.node, limits, next);
            }
        }
        // Cut off, it's gone over again next, past what's gone already.
        if (limits.cut) {
            break;
        }
        if (whole) {
            held.whole.insert_or_assign(want.node, want);
            held.waiting.erase(want.node);
        } else {
            held.waiting.insert_or_assign(want.node, want);
            next.short_of.push_back(
                {.want = want, .rank = {.round = held.round, .priority = want.priority}});
        }
    }
    held.due.erase(held.due.begin(), held.due.begin() + static_cast<std::ptrdiff_t>(done));
    return next;
}

std::size_t ChunkSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
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
