#pragma once

#include <hog/bot/wander.hpp>
#include <hog/client/player.hpp>
#include <hog/client/session.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/world.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace hog::bot {

// A headless player for testing servers under live load: it connects like a client, walks the
// ground like one, and wanders between spots near where it started.
class Bot {
public:
    // Stood on the ground under a unit `direction`, connecting to `address` with `token` from
    // the login service, or none. The world must outlive the bot.
    [[nodiscard]] static Result<Bot> start(const planet::World& world, std::string address,
                                           std::vector<std::byte> token, DVec3 direction,
                                           std::uint64_t seed, const WanderSettings& settings = {});

    // A frame: takes in what the server sent, wanders on, and tells it where the feet are.
    void update(MonotonicClock::time_point now, double seconds);

    [[nodiscard]] WorldPosition feet() const { return player_.feet(); }
    [[nodiscard]] const client::Session& session() const noexcept { return session_; }

private:
    Bot(client::Player player, client::Session session, double planet_radius, std::uint64_t seed,
        const WanderSettings& settings) noexcept;

    client::Player player_;
    client::Session session_;
    Wander wander_;
    std::mt19937_64 random_;
    double planet_radius_;
    WanderSettings settings_;
};

} // namespace hog::bot
