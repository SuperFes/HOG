#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/net/events.hpp>
#include <hog/physics/space.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/replication/view.hpp>
#include <hog/server/move_check.hpp>
#include <hog/server/spatial_index.hpp>

#include <flecs.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hog::server {

// An entity others are told about: its id on the wire and its name.
struct Replicated {
    protocol::NetId id{};
    std::string name;
};

enum class Ruling : std::uint8_t { Taken, Stale, NotJoined, OutOfBounds, TooFast, UnderGround };

// How a ruling reads in a log: "too fast", say.
[[nodiscard]] std::string_view describe(Ruling ruling) noexcept;

// What came of a move a client sent, and the correction to send it back if it wasn't taken.
struct MoveResult {
    Ruling ruling = Ruling::Taken;
    std::optional<protocol::Corrected> correction;
};

// A player with an account, and where they were, to be placed there when they come back.
struct Remembered {
    std::uint64_t account = 0;
    protocol::Placed place;
};

// The players' entities, by their connections, and what each is shown of those near them. A
// player who leaves is gone from the world, but where an account's player was is handed back to
// be kept.
class Players {
public:
    // Players may be from half to twice `radius` from the planet's centre. The world must
    // outlive this.
    Players(flecs::world& world, double radius, const replication::ViewSettings& view = {},
            const MoveRules& rules = {}) noexcept;

    // Makes a player's entity, with no Position until it first moves, remembered on leaving if
    // it has an `account`. Precondition: the connection hasn't joined.
    flecs::entity join(net::ConnectionId connection, std::string name,
                       std::optional<std::uint64_t> account = std::nullopt);
    // A player flying goes anywhere within bounds, and the first move on foot after flying or
    // joining lands them anywhere within bounds. Later ones are checked against the time since
    // the last taken, and the ground in `space`; one that isn't taken leaves them where they
    // were, to be sent back there. Moves older than the newest seen, or made before the client
    // took the newest correction, are left out.
    MoveResult move(net::ConnectionId connection, const protocol::Moved& moved,
                    MonotonicClock::time_point now, const physics::Space& space);

    // Removes the player's entity, and says where they were if they have an account and a
    // place; nothing for a connection that hasn't joined.
    std::optional<Remembered> leave(net::ConnectionId connection);
    // Where each player in with an account and a place is.
    [[nodiscard]] std::vector<Remembered> remembered() const;

    // Does nothing for a connection that hasn't joined.
    void acknowledged(net::ConnectionId connection, std::uint32_t tick) noexcept;

    // Calls send(connection, snapshot) with each placed player's next snapshot, of the other
    // replicated entities `index` finds near them.
    template <typename Send>
        requires std::invocable<Send&, net::ConnectionId, const protocol::Snapshot&>
    void share(const SpatialIndex& index, Send send) {
        for (auto& [connection, joined] : joined_) {
            if (const auto snapshot = next(joined, index)) {
                send(connection, *snapshot);
            }
        }
    }

    [[nodiscard]] std::optional<flecs::entity> find(net::ConnectionId connection) const;
    [[nodiscard]] std::size_t size() const noexcept { return joined_.size(); }

    // Where each player on foot is.
    [[nodiscard]] std::vector<WorldPosition> placed() const;
    // Every player's connection, placed or not.
    [[nodiscard]] std::vector<net::ConnectionId> connections() const;

private:
    struct Joined {
        flecs::entity entity;
        replication::View view;
        std::optional<std::uint64_t> account;
        std::optional<MoveCheck> check;         // on foot, once landed
        std::optional<protocol::Placed> placed; // as last taken
        std::uint32_t sequence = 0;             // of the newest move seen
        std::uint32_t corrections = 0;          // sent
    };

    // None for a player with no Position yet.
    [[nodiscard]] std::optional<protocol::Snapshot> next(Joined& joined, const SpatialIndex& index);

    flecs::world* world_;
    double nearest_;
    double farthest_;
    replication::ViewSettings view_;
    MoveRules rules_;
    std::map<net::ConnectionId, Joined> joined_;
    // Reused from one snapshot to the next.
    std::vector<replication::Nearby> nearby_;
    // Ids aren't reused while the server runs, so a late snapshot never names the wrong entity.
    std::uint32_t next_id_ = 1;
};

} // namespace hog::server
