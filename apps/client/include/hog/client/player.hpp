#pragma once

#include <hog/client/flight.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/physics/character.hpp>
#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>
#include <hog/planet/world.hpp>

#include <memory>

namespace hog::client {

struct PlaySettings {
    physics::CharacterSettings character{};
    double shoulder = 1.6;  // metres over the feet that the camera turns about
    double clearance = 0.2; // metres the camera keeps off the ground
};

// The player's own character, walking on the ground near it.
class Player {
public:
    // Stood on the ground under a unit `direction`.
    [[nodiscard]] static Result<Player> create(const planet::World& world, DVec3 direction,
                                               const PlaySettings& settings = {});

    // Moves on by `seconds`, with the ground kept round wherever it gets to.
    Result<> play(const physics::Stride& stride, double seconds);

    // Puts the feet somewhere, at rest, as the server says.
    void place(WorldPosition feet);

    [[nodiscard]] WorldPosition feet() const { return character_.feet(); }
    [[nodiscard]] bool standing() const { return character_.standing(); }
    // Where the camera turns about.
    [[nodiscard]] WorldPosition shoulder() const;
    // Metres from `from` towards a unit `back`, up to `wanted`, that the camera can go before
    // it's too near the ground.
    [[nodiscard]] double room(WorldPosition from, DVec3 back, double wanted) const;

private:
    Player(std::unique_ptr<physics::Space> space, physics::Ground ground,
           physics::Character character, const PlaySettings& settings);

    // On the heap, where the ground can keep hold of it as the player moves.
    std::unique_ptr<physics::Space> space_;
    physics::Ground ground_;
    physics::Character character_;
    PlaySettings settings_;
};

// What the keys held ask of the character: `move` is right, up and forwards, each from -1 to 1,
// facing the way `aim` does along the ground; up jumps.
[[nodiscard]] physics::Stride stride(const Flight& aim, DVec3 move, bool running);

} // namespace hog::client
