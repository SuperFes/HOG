#include <hog/server/piece_sending.hpp>

#include <algorithm>
#include <utility>

namespace hog::server {
namespace {

// The bodies of `marked` that `told` hadn't, now in it.
std::vector<grid::Water::Value> newly_told(std::set<grid::Water::Value>& told,
                                           std::span<const grid::Water::Value> marked) {
    std::vector<grid::Water::Value> bodies;
    for (const grid::Water::Value body : marked) {
        if (body != 0 && told.insert(body).second) {
            bodies.push_back(body);
        }
    }
    return bodies;
}

// The answer's piece with only the kinds read from other chunks than `held` were.
grid::Piece changed_since(const PieceAnswer& answer, const PieceAnswer::Stamps& held) {
    grid::Piece piece;
    piece.root = answer.piece.root;
    piece.generator = answer.piece.generator;
    if (answer.stamps.shape != held.shape) {
        piece.shape = answer.piece.shape;
    }
    if (answer.stamps.material != held.material) {
        piece.material = answer.piece.material;
    }
    if (answer.stamps.water != held.water) {
        piece.water = answer.piece.water;
    }
    if (answer.stamps.woods != held.woods) {
        piece.woods = answer.piece.woods;
    }
    return piece;
}

} // namespace

void PieceSending::join(net::ConnectionId player) {
    players_.try_emplace(player);
}

void PieceSending::leave(net::ConnectionId player) {
    players_.erase(player);
}

void PieceSending::ask(net::ConnectionId player, std::span<const Want> wants) {
    Player& held = players_[player];
    held.round = ++rounds_;
    held.due.clear();
    held.waiting.clear();
    for (Want want : wants) {
        if (!want.node.is_valid() || want.node.depth > finest_) {
            continue;
        }
        want.depth = std::clamp(want.depth, want.node.depth, finest_);
        const auto had = held.held.find(want.node);
        if (had == held.held.end() || had->second.depth < want.depth ||
            (had->second.depth == want.depth && !had->second.whole)) {
            held.due.push_back(want);
        }
    }
}

void PieceSending::due_again(Player& player, const Want& want) {
    if (std::ranges::any_of(player.due, [&](const Want& due) { return due.node == want.node; })) {
        return;
    }
    player.due.insert(
        std::ranges::find_if(player.due,
                             [&](const Want& due) { return due.priority < want.priority; }),
        want);
}

void PieceSending::changed(std::span<const PieceSource::Touched> touched) {
    for (auto& [id, player] : players_) {
        std::vector<Want> first;
        for (const PieceSource::Touched& each : touched) {
            std::vector<Want> woken;
            grid::for_nested(player.waiting, each.node, [&](grid::Key node, const Want& want) {
                if (PieceSource::overlaps(each, node, want.depth)) {
                    woken.push_back(want);
                }
            });
            for (const Want& want : woken) {
                player.waiting.erase(want.node);
                due_again(player, want);
            }
            grid::for_nested(player.held, each.node, [&](grid::Key node, const Holding& holding) {
                const auto same = [&](const Want& want) { return want.node == node; };
                if (PieceSource::overlaps(each, node, holding.depth) &&
                    !std::ranges::any_of(player.due, same) && !std::ranges::any_of(first, same)) {
                    first.push_back(
                        {.node = node, .depth = holding.depth, .priority = holding.priority});
                }
            });
        }
        player.due.insert(player.due.begin(), first.begin(), first.end());
    }
}

std::vector<PieceSending::Sent> PieceSending::next(net::ConnectionId player, std::size_t room,
                                                   PieceSource& source, std::size_t most) {
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
        const std::shared_ptr<const PieceAnswer> answer =
            source.answer(want.node, want.depth, {.round = held.round, .priority = want.priority});
        if (!answer) {
            held.waiting[want.node] = want;
            continue;
        }
        if (!answer->whole) {
            held.waiting[want.node] = want;
        }
        // Deeper than held, the answer goes as it's kept; as deep, only the kinds changed of it.
        const auto had = held.held.find(want.node);
        std::vector<std::byte> coded;
        if (had == held.held.end() || had->second.depth < want.depth) {
            coded = answer->bytes;
        } else if (had->second.depth == want.depth) {
            const grid::Piece newer = changed_since(*answer, had->second.stamps);
            if (!newer.shape && !newer.material && !newer.water && !newer.woods) {
                continue;
            }
            coded = newer.encode();
        } else {
            continue;
        }
        if (!sent.empty() && bytes + coded.size() > room) {
            bytes = room;
            held.waiting.erase(want.node);
            still.push_back(want);
            continue;
        }
        bytes += coded.size();
        held.held[want.node] = {.depth = want.depth,
                                .stamps = answer->stamps,
                                .whole = answer->whole,
                                .priority = want.priority};
        sent.push_back({.node = want.node,
                        .bytes = std::move(coded),
                        .bodies = newly_told(held.told, answer->bodies)});
    }
    held.due = std::move(still);
    return sent;
}

std::size_t PieceSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

std::size_t PieceSending::waiting(net::ConnectionId player) const {
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
