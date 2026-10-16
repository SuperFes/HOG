#pragma once

#include <hog/core/math.hpp>
#include <hog/physics/space.hpp>

#include <memory>

namespace hog::physics {

struct CharacterSettings {
    double height = 1.8;    // metres, feet to the top of the head
    double radius = 0.3;    // metres
    double walking = 1.5;   // metres a second
    double running = 6.0;   // metres a second
    double jumping = 4.5;   // metres a second upwards, leaving the ground
    double steepest = 0.87; // radians from level that can be walked up
    double step = 0.4;      // metres it steps up onto without jumping
    double gravity = 9.81;  // metres a second squared, towards the centre
};

// What a player asks of their character.
struct Stride {
    DVec3 along{0.0}; // the way to go, at most unit for full speed; any part along up is ignored
    bool running = false;
    bool jumping = false;
};

// Someone walking about a planet centred on the origin: up is always away from its centre, and
// gravity towards it.
class Character {
public:
    static constexpr double step_seconds = 1.0 / 60.0;

    Character(Space& space, WorldPosition feet, const CharacterSettings& settings = {});
    ~Character();
    Character(Character&&) noexcept;
    Character& operator=(Character&&) noexcept;
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    // Moves on through whole steps of `seconds` and what was left over from before, so the same
    // strides give the same moves at any frame rate.
    void move(const Stride& stride, double seconds);

    // Puts the character's feet somewhere, at rest.
    void place(WorldPosition feet);

    [[nodiscard]] WorldPosition feet() const;
    // Metres a second it moved over its last step.
    [[nodiscard]] DVec3 velocity() const;
    // On ground that isn't too steep to stand on.
    [[nodiscard]] bool standing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::physics
