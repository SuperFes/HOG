#include <hog/globe/cell.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/column_sending.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace hog::server {
namespace {

constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
// A tile column is a cell this many levels coarser than a voxel's.
constexpr int coarser = std::countr_zero(side);

} // namespace

void ColumnSending::join(net::ConnectionId player) {
    players_.insert_or_assign(player, Player{});
}

void ColumnSending::leave(net::ConnectionId player) {
    players_.erase(player);
}

void ColumnSending::follow(net::ConnectionId player, DVec3 at) {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return;
    }
    Player& one = found->second;
    const globe::Cell under = globe::cell_under(glm::normalize(at), layers_.level - coarser);
    const globe::Site over{.face = under.face, .i = under.i * side, .j = under.j * side};
    if (one.over == over) {
        return;
    }
    one.over = over;
    struct Near {
        double metres = 0.0;
        Due due;
    };
    std::vector<Near> near;
    const double radius = glm::length(at);
    const std::vector<globe::Site> in_reach = globe::tile_columns_near(layers_, at, reach);
    // Out of reach the player lets them go, to be sent again on coming back.
    std::erase_if(one.held, [&](const auto& held) {
        return !std::ranges::binary_search(in_reach, held.first);
    });
    for (const globe::Site corner : in_reach) {
        const globe::Cell cell{.level = static_cast<std::uint8_t>(layers_.level - coarser),
                               .face = corner.face,
                               .i = corner.i / side,
                               .j = corner.j / side};
        const double metres = glm::length((globe::center(cell) * radius) - at);
        const std::uint8_t level = globe::level_at(metres);
        const auto held = one.held.find(corner);
        if (held == one.held.end() || held->second != level) {
            near.push_back({.metres = metres, .due = {.corner = corner, .level = level}});
        }
    }
    std::ranges::sort(near, {}, &Near::metres);
    one.due.clear();
    one.due.reserve(near.size());
    for (const Near& each : near) {
        one.due.push_back(each.due);
    }
}

void ColumnSending::changed(std::span<const globe::Site> corners) {
    for (auto& [id, player] : players_) {
        std::vector<Due> first;
        for (const globe::Site corner : corners) {
            const auto held = player.held.find(corner);
            if (held != player.held.end() && held->second == 0) {
                std::erase_if(player.due, [&](const Due& due) { return due.corner == corner; });
                first.push_back({.corner = corner, .level = 0});
            }
        }
        player.due.insert(player.due.begin(), first.begin(), first.end());
    }
}

Result<protocol::SentColumn> ColumnSending::read(const Due& due, const globe::Volume& volume,
                                                 const globe::VolumeArchive& archive) {
    protocol::SentColumn sent{.face = due.corner.face,
                              .i = due.corner.i / side,
                              .j = due.corner.j / side,
                              .level = due.level,
                              .bytes = {}};
    if (due.level == 0 && volume.floor(due.corner)) {
        sent.bytes = volume.encode_column(due.corner);
        return sent;
    }
    auto saved = archive.column(sent.face, sent.i, sent.j, sent.level);
    if (!saved) {
        return std::unexpected(std::move(saved).error());
    }
    if (*saved) {
        sent.bytes = std::move((*saved)->bytes);
    }
    return sent;
}

Result<protocol::Columns> ColumnSending::next(net::ConnectionId player, std::size_t room,
                                              const globe::Volume& volume,
                                              const globe::VolumeArchive& archive) {
    protocol::Columns columns;
    const auto found = players_.find(player);
    if (found == players_.end() || room == 0) {
        return columns;
    }
    Player& one = found->second;
    std::size_t bytes = 0;
    std::size_t taken = 0;
    for (; taken < one.due.size() && columns.columns.size() < protocol::most_columns; ++taken) {
        Due& due = one.due[taken];
        auto sent = read(due, volume, archive);
        // Too big for a message: sent coarser, which always fits in time.
        while (sent && sent->bytes.size() > protocol::most_column &&
               due.level < globe::coarsest_level) {
            ++due.level;
            sent = read(due, volume, archive);
        }
        if (!sent) {
            return std::unexpected(std::move(sent).error());
        }
        if (!columns.columns.empty() && (bytes + sent->bytes.size() > room ||
                                         bytes + sent->bytes.size() > protocol::most_column)) {
            break;
        }
        bytes += sent->bytes.size();
        one.held.insert_or_assign(due.corner, due.level);
        columns.columns.push_back(std::move(*sent));
    }
    one.due.erase(one.due.begin(), one.due.begin() + static_cast<std::ptrdiff_t>(taken));
    return columns;
}

std::size_t ColumnSending::due(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? 0 : found->second.due.size();
}

} // namespace hog::server
