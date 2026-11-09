#pragma once

#include <hog/bot/wander.hpp>
#include <hog/client/player.hpp>
#include <hog/client/session.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace hog::bot {

// A headless player for testing servers under live load: it connects like a client, walks the
// ground the server sends it like one, and wanders between spots near where it started.
class Bot {
public:
    // To stand on the ground under a unit `direction` once it's sent, connecting to `address`
    // with `token` from the login service, or none. The generator must outlive the bot.
    [[nodiscard]] static Result<Bot> start(const globe::Generator& generator, std::string address,
                                           std::vector<std::byte> token, DVec3 direction,
                                           std::uint64_t seed, const WanderSettings& settings = {});

    // A frame: takes in what the server sent, wanders on, and tells it where the feet are.
    void update(MonotonicClock::time_point now, double seconds);

    // Where it stands, or is to once the ground there is sent.
    [[nodiscard]] WorldPosition feet() const;
    [[nodiscard]] bool standing() const noexcept { return player_.has_value(); }
    [[nodiscard]] const client::Session& session() const noexcept { return session_; }
    // The ground as it's been sent.
    [[nodiscard]] const globe::World& world() const noexcept { return held_->world; }

private:
    // On the heap, where the player's ground can keep hold of it.
    struct Held {
        explicit Held(const globe::Generator& generator) : world{generator, volume} {
            world.read_tiles(&tiles);
        }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
        Held(Held&&) = delete;
        Held& operator=(Held&&) = delete;
        ~Held() = default;

        globe::Volume volume; // none: the ground is the tiles sent
        globe::HeldTiles tiles;
        globe::World world;
    };

    Bot(std::unique_ptr<Held> held, client::Session session, DVec3 direction, std::uint64_t seed,
        const WanderSettings& settings) noexcept;
    // Takes in the tiles sent, standing the player on the ground once its finest is there.
    void take_ground();

    std::unique_ptr<Held> held_;
    std::optional<client::Player> player_;
    DVec3 direction_;
    client::Session session_;
    Wander wander_;
    std::mt19937_64 random_;
    double planet_radius_;
    WanderSettings settings_;
};

} // namespace hog::bot
