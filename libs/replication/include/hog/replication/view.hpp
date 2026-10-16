#pragma once

#include <hog/protocol/messages.hpp>
#include <hog/replication/states.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace hog::replication {

struct ViewSettings {
    double radius = 500.0; // metres within which entities come into view
    // Times the radius past which they leave it, so one at the edge doesn't flicker.
    double leave_at = 1.1;
    std::size_t budget = 1'200; // bytes a snapshot keeps under, as far as it can
};

// An entity near a client, for its view.
struct Nearby {
    protocol::NetId id{};
    protocol::Millimetres at{0};
    std::string_view name;
    double distance = 0.0; // metres from the client
};

// What one client is shown: each snapshot is coded against the newest the client acknowledged,
// and keeps under the budget by leaving out the farthest entities' entering and changes until a
// later one. The view records what the client will hold, not what's true, so an entity left out
// stays as the client has it.
class View {
public:
    // Snapshots kept to code against: 1.6 seconds at 20 a second.
    static constexpr std::size_t kept = 32;

    explicit View(const ViewSettings& settings) noexcept : settings_(settings) {}

    // The next snapshot, of the entities within leaving distance, in any order.
    [[nodiscard]] protocol::Snapshot next(std::span<const Nearby> nearby);
    // Ignores an ack of a snapshot not sent, or older than one acknowledged already.
    void acknowledged(std::uint32_t tick) noexcept;

    // The last snapshot's.
    [[nodiscard]] std::uint32_t tick() const noexcept { return tick_; }
    // What the client holds once it has the last snapshot.
    [[nodiscard]] const EntityStates& sent() const noexcept;

private:
    ViewSettings settings_;
    std::uint32_t tick_ = 0;
    std::optional<std::uint32_t> acknowledged_;
    // Oldest first.
    std::deque<std::pair<std::uint32_t, EntityStates>> sent_;
};

} // namespace hog::replication
