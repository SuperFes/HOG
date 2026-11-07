#pragma once

#include <hog/client/grains.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/net/client.hpp>
#include <hog/net/events.hpp>
#include <hog/net/stats.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/replication/replica.hpp>
#include <hog/replication/timeline.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hog::client {

// A line of chat as the server relays it: who said it, and what.
struct ChatLine {
    std::string from;
    std::string text;

    friend bool operator==(const ChatLine&, const ChatLine&) = default;
};

// Where the player is, as the server is told: their feet on foot, or their eye flying.
struct Whereabouts {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition at = {};
    protocol::Stance stance = protocol::Stance::Walking;
    protocol::Facing facing{};
};

// A player's connection to the game server: says hello, then keeps what's near them from its
// snapshots and tells it where their feet are, taking it back when the server won't have it.
class Session {
public:
    enum class State : std::uint8_t { Connecting, Welcomed, Refused, Closed };

    // Real time between the moves sent, at most.
    static constexpr std::chrono::milliseconds move_interval{50};

    // Starts connecting to `address`, to say hello with the login service's token, or none.
    // Given the id of the planet the client makes ground of, it's refused a server of another.
    [[nodiscard]] static Result<Session> connect(std::string address, std::vector<std::byte> token,
                                                 std::optional<std::uint64_t> world = std::nullopt);

    // Takes in what the server sent, answering it, and once welcomed tells it where the player
    // is, when given.
    void look(std::optional<Whereabouts> player, MonotonicClock::time_point now);

    // Where the server put the player back to since last asked, once for each time.
    [[nodiscard]] std::optional<WorldPosition> take_correction() noexcept {
        return std::exchange(correction_, std::nullopt);
    }
    // Where the server says the player was when they last left, once.
    [[nodiscard]] std::optional<protocol::Placed> take_placed() noexcept {
        return std::exchange(placed_, std::nullopt);
    }

    // The newest weather the server sent since last asked, once.
    [[nodiscard]] std::optional<protocol::Weather> take_weather() noexcept {
        return std::exchange(weather_, std::nullopt);
    }
    // Says `text` to everyone, tidied to what chat may carry; nothing of it before being
    // welcomed, or once tidied to nothing.
    void say(std::string_view text);

    // Asks the server to run the world at `scale` game seconds a real one; nothing before being
    // welcomed, or of a scale of nothing.
    void hurry(std::uint32_t scale);

    // Game seconds a real one the server says its world runs at, 1 until it says otherwise.
    [[nodiscard]] std::uint32_t world_rate() const noexcept { return world_rate_; }

    // The chat heard since last asked, oldest first.
    [[nodiscard]] std::vector<ChatLine> take_chat() noexcept { return std::exchange(chat_, {}); }

    // Tile columns of the voxel world the server sent since last asked, oldest first, each to
    // hold in place of what was held of it.
    [[nodiscard]] std::vector<protocol::SentColumn> take_columns() noexcept {
        return std::exchange(columns_, {});
    }

    // The grain the server has in flight near the player.
    [[nodiscard]] const GrainField& grains() const noexcept { return grains_; }

    // Asks the server to dig out, or build up of `material`, a ball `radius` metres across at
    // `centre`; nothing before being welcomed, or of a ball of no width or wider than it allows.
    void dig(WorldPosition centre, double radius);
    void build(WorldPosition centre, double radius, std::uint8_t material);

    // The planet the server runs, as it said when it welcomed; empty before.
    [[nodiscard]] const std::vector<std::byte>& planet() const noexcept { return planet_; }
    // The voxel layers the server's ground stands on, as hog::globe::volume_header has them.
    [[nodiscard]] const std::vector<std::byte>& ground_origin() const noexcept {
        return ground_origin_;
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string& address() const noexcept { return address_; }
    // Why the server refused or closed the connection.
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] const replication::Replica& replica() const noexcept { return replica_; }
    // Others as they're shown at `now`: a little in the past, between the snapshots around then.
    [[nodiscard]] std::vector<replication::Shown> others(MonotonicClock::time_point now) const {
        return timeline_.at(now);
    }
    [[nodiscard]] std::optional<net::Stats> stats() const { return client_.stats(); }
    // When the newest snapshot came; none before the first.
    [[nodiscard]] std::optional<MonotonicClock::time_point> last_snapshot() const noexcept {
        return last_snapshot_;
    }
    // Snapshots taken in over the last whole second.
    [[nodiscard]] std::uint32_t snapshot_rate() const noexcept { return rate_; }

private:
    Session(net::Client client, std::string address, std::vector<std::byte> token) noexcept;

    void heard(std::span<const std::byte> bytes, MonotonicClock::time_point now);
    void send(const protocol::Message& message, net::Delivery delivery);

    net::Client client_;
    std::string address_;
    std::vector<std::byte> token_;
    std::optional<std::uint64_t> world_;
    std::vector<std::byte> planet_;
    std::vector<std::byte> ground_origin_;
    State state_ = State::Connecting;
    std::string reason_;
    replication::Replica replica_;
    replication::Timeline timeline_;
    std::vector<net::Event> events_;
    std::optional<MonotonicClock::time_point> next_move_;
    std::uint32_t moves_ = 0;
    std::uint32_t corrections_ = 0;
    std::optional<WorldPosition> correction_;
    std::optional<protocol::Placed> placed_;
    std::optional<protocol::Weather> weather_;
    std::vector<ChatLine> chat_;
    std::vector<protocol::SentColumn> columns_;
    GrainField grains_;
    std::uint32_t world_rate_ = 1;
    std::optional<MonotonicClock::time_point> last_snapshot_;
    MonotonicClock::time_point counting_since_;
    std::uint32_t counted_ = 0;
    std::uint32_t rate_ = 0;
};

// What the network window shows of a session.
struct SessionSummary {
    struct Seen {
        std::string name;
        double distance = 0.0; // metres from the eye
    };

    std::string connection;   // address and state, with why it was refused or closed
    std::string quality;      // ping and loss, once known
    std::string traffic;      // once known
    std::string snapshots;    // how often they come, and the newest's age
    std::string clock;        // the world's speed, when not at real time
    std::vector<Seen> nearby; // nearest first
};

[[nodiscard]] SessionSummary summarise(const Session& session, WorldPosition eye,
                                       MonotonicClock::time_point now);

} // namespace hog::client
