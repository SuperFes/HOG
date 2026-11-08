#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace hog::server {

// The tile columns each player is sent: every one within reach of them, finer the nearer, the
// nearest first. A player holds a column at the level last sent, until sent it at another.
class ColumnSending {
public:
    // Metres from a player its ground is sent, each tile column at globe::level_at its distance.
    static constexpr double reach = 8000.0;

    explicit ColumnSending(const globe::Layers& layers) : layers_{layers} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);
    // Where the player is, from the planet's centre: what it's to be sent is worked out afresh
    // when it's over another tile column than before, and what's out of reach no longer counts
    // as held.
    void follow(net::ConnectionId player, DVec3 at);
    // The world changed the tile columns at these corners: sent again first to whoever holds
    // them at their own level.
    void changed(std::span<const globe::Site> corners);

    // The next tile columns for the player, nearest first, while they come to under `room`
    // bytes, and one at least if any is due: its own level read from `volume` where held, else
    // from `archive`, where coarser levels always are. Those sent count as held.
    [[nodiscard]] Result<protocol::Columns> next(net::ConnectionId player, std::size_t room,
                                                 const globe::Volume& volume,
                                                 const globe::VolumeArchive& archive);

    // Tile columns still due to the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;

private:
    struct Due {
        globe::Site corner;
        std::uint8_t level = 0;
    };
    struct Player {
        std::optional<globe::Site> over;          // the tile column it was last over
        std::map<globe::Site, std::uint8_t> held; // corner and level sent
        std::vector<Due> due;                     // nearest first
    };

    [[nodiscard]] static Result<protocol::SentColumn>
    read(const Due& due, const globe::Volume& volume, const globe::VolumeArchive& archive);

    globe::Layers layers_;
    std::map<net::ConnectionId, Player> players_;
};

} // namespace hog::server
