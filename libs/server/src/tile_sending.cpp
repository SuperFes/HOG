#include <hog/globe/cell.hpp>
#include <hog/server/tile_sending.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace hog::server {
namespace {

constexpr int side = globe::Volume::tile_side;
// A tile is a cell this many levels coarser than its voxels'.
constexpr int coarser = std::countr_zero(static_cast<unsigned>(side));

// Whether any of the tile at `key` is in the shell's layers.
bool in_shell(const globe::Layers& layers, globe::TileKey key) {
    return static_cast<std::int64_t>(key.k) * (std::int64_t{side} << key.level) < layers.count;
}

// The finest tile holding `at`, its layer kept within the shell.
globe::TileKey finest_at(const globe::Layers& layers, DVec3 at) {
    const globe::Cell cell = globe::cell_under(glm::normalize(at), layers.level - coarser);
    const int layer = std::clamp(layers.layer_at(glm::length(at)), 0, layers.count - 1);
    return {
        .face = cell.face, .i = cell.i, .j = cell.j, .k = static_cast<std::uint32_t>(layer / side)};
}

} // namespace

double metres_to(const globe::Layers& layers, globe::TileKey key, DVec3 at) {
    const globe::Cell cell{.level = static_cast<std::uint8_t>(layers.level - coarser - key.level),
                           .face = key.face,
                           .i = key.i,
                           .j = key.j};
    const DVec3 middle = globe::center(cell);
    const double radius = glm::length(at);
    const DVec3 up = at / radius;
    double half = 0.0;
    for (const DVec3& corner : globe::corners(cell)) {
        half = std::max(half, std::acos(std::clamp(glm::dot(middle, corner), -1.0, 1.0)));
    }
    const double away = std::acos(std::clamp(glm::dot(middle, up), -1.0, 1.0));
    const double across = std::max(0.0, away - half) * radius;
    const int span = side << key.level;
    const double low = layers.bottom(static_cast<int>(key.k) * span);
    const double high = layers.bottom(std::min((static_cast<int>(key.k) + 1) * span, layers.count));
    const double rise = std::max({0.0, low - radius, radius - high});
    return std::hypot(across, rise);
}

std::uint8_t TileSending::level_at(double metres, std::uint8_t top) const noexcept {
    if (metres < reach_) {
        return 0;
    }
    const double doublings = std::floor(std::log2(metres / reach_));
    return static_cast<std::uint8_t>(std::min(1.0 + doublings, static_cast<double>(top)));
}

std::vector<globe::TileKey> TileSending::wanted(DVec3 at, globe::Land& land,
                                                const globe::EachOf& each) const {
    const globe::Layers& layers = land.generator().layers();
    std::vector<std::pair<double, globe::TileKey>> chosen;
    std::vector<globe::TileKey> frontier;
    for (std::uint8_t face = 0; face < 6; ++face) {
        for (globe::TileKey root{.level = land.top(), .face = face}; in_shell(layers, root);
             ++root.k) {
            frontier.push_back(root);
        }
    }
    // A level at a time, so each level's tiles are made together.
    while (!frontier.empty()) {
        land.make(frontier, each);
        std::vector<globe::TileKey> finer;
        std::vector<double> away;
        for (const globe::TileKey key : frontier) {
            const double metres = metres_to(layers, key, at);
            if (key.level == 0 || level_at(metres, land.top()) >= key.level) {
                chosen.emplace_back(metres, key);
            } else {
                finer.push_back(key);
                away.push_back(metres);
            }
        }
        frontier.clear();
        for (std::size_t n = 0; n < finer.size(); ++n) {
            const globe::TileKey key = finer[n];
            if (land.hidden(key)) {
                chosen.emplace_back(away[n], key);
                continue;
            }
            for (std::size_t octant = 0; octant < 8; ++octant) {
                if (const globe::TileKey child = key.child(octant); in_shell(layers, child)) {
                    frontier.push_back(child);
                }
            }
        }
    }
    std::ranges::sort(chosen);
    std::vector<globe::TileKey> keys;
    keys.reserve(chosen.size());
    for (const auto& [metres, key] : chosen) {
        keys.push_back(key);
    }
    return keys;
}

void TileSending::join(net::ConnectionId player) {
    players_.insert_or_assign(player, Player{});
}

void TileSending::leave(net::ConnectionId player) {
    players_.erase(player);
}

bool TileSending::moved(net::ConnectionId player, DVec3 at, const globe::Layers& layers) {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return false;
    }
    const globe::TileKey over = finest_at(layers, at);
    return std::exchange(found->second.over, over) != over;
}

std::set<globe::TileKey> TileSending::held(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? std::set<globe::TileKey>{} : found->second.held;
}

TileSending::Cut TileSending::cut(DVec3 at, const std::set<globe::TileKey>& held, globe::Land& land,
                                  const globe::EachOf& each) const {
    Cut made;
    made.keys = wanted(at, land, each);
    std::vector<globe::TileKey> coding;
    for (const globe::TileKey key : made.keys) {
        if (!held.contains(key)) {
            coding.push_back(key);
        }
    }
    // All made, the land is only read now, so it's read side by side.
    std::vector<std::vector<std::byte>> coded(coding.size());
    const auto body = [&](std::size_t n) { coded[n] = land.made(coding[n]).encode(); };
    if (each) {
        each(coding.size(), body);
    } else {
        for (std::size_t n = 0; n < coding.size(); ++n) {
            body(n);
        }
    }
    for (std::size_t n = 0; n < coding.size(); ++n) {
        if (coded[n].size() > protocol::most_tile) {
            ++made.too_big;
            continue;
        }
        const globe::TileKey key = coding[n];
        made.sent.push_back({.level = key.level,
                             .face = key.face,
                             .i = key.i,
                             .j = key.j,
                             .k = key.k,
                             .bytes = std::move(coded[n])});
    }
    return made;
}

void TileSending::take(net::ConnectionId player, Cut cut) {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return;
    }
    Player& one = found->second;
    const std::set<globe::TileKey> kept{cut.keys.begin(), cut.keys.end()};
    // What's cut otherwise the player lets go, to be sent again when cut so again.
    std::erase_if(one.held, [&](const globe::TileKey key) { return !kept.contains(key); });
    one.due = std::move(cut.sent);
}

void TileSending::follow(net::ConnectionId player, DVec3 at, globe::Land& land,
                         const globe::EachOf& each) {
    if (moved(player, at, land.generator().layers())) {
        take(player, cut(at, held(player), land, each));
    }
}

protocol::Tiles TileSending::next(net::ConnectionId player, std::size_t room) {
    protocol::Tiles out;
    const auto found = players_.find(player);
    if (found == players_.end() || room == 0) {
        return out;
    }
    Player& one = found->second;
    std::size_t bytes = 0;
    std::size_t taken = 0;
    for (; taken < one.due.size() && out.tiles.size() < protocol::most_tiles; ++taken) {
        protocol::SentTile& tile = one.due[taken];
        if (!out.tiles.empty() &&
            (bytes + tile.bytes.size() > room || bytes + tile.bytes.size() > protocol::most_tile)) {
            break;
        }
        bytes += tile.bytes.size();
        one.held.insert(
            {.level = tile.level, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k});
        out.tiles.push_back(std::move(tile));
    }
    one.due.erase(one.due.begin(), one.due.begin() + static_cast<std::ptrdiff_t>(taken));
    return out;
}

std::size_t TileSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

} // namespace hog::server
