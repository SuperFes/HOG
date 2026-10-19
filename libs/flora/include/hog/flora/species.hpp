#pragma once

#include <array>

namespace hog::flora {

// How one level of branching grows; level 0 is the trunk, each level sprouting off the one
// before.
struct Level {
    float length = 10.0F; // metres a branch of this level runs
    float sway = 0.2F;    // ± fraction of length each branch differs by
    float radius = 0.5F;  // of the parent's radius where it sprouts; the trunk's in metres
    float taper = 0.8F;   // of the radius lost from base to tip
    int sections = 8;     // rings a branch is built from
    int sides = 6;        // vertices around each ring
    int children = 0;     // branches sprouted onto the next level
    int whorl = 1;        // children sharing each height, ringing the parent
    float start = 0.3F;   // of the parent's length before children sprout
    float angle = 1.0F;   // radians a child leans off its parent
    float fall = 0.0F;    // of a child's length lost by the parent's tip, 0 for an even crown
    float gnarl = 0.1F;   // radians a branch wanders each section
    float rise = 0.0F;    // pull toward upright each section; negative droops
};

// A species is a small rule set the grammar grows an individual from, deterministically from a
// seed: the same on every machine, as many times as it's grown.
struct Species {
    std::array<Level, 3> levels;
    int grown = 2;             // levels grown, 1 to 3
    float frond_width = 0.25F; // a frond's base half-width, as a fraction of its twig's length
    float frond_sway = 0.25F;  // ± fraction of frond_width each frond differs by
    float frond_tilt = 0.5F;   // radians each of a frond's two blades rolls off flat
};

// The pine analogue: whorled boughs off a single trunk, a conical crown.
[[nodiscard]] Species pine();

// The age a species' rules describe it at, in its prime; 0 is a sapling, 1 old.
inline constexpr float prime_age = 0.7F;

// The species as it grows at `age`, 0 to 1: a sapling is a short, thin, upswept cone of a few
// whorls down to the ground; an old tree has a stout trunk, its lower boughs shed, its crown raised
// and rounded, its boughs drooping. Exactly the species at prime_age; pure arithmetic, so as
// bit-stable as grow().
[[nodiscard]] Species aged(const Species& species, float age) noexcept;

} // namespace hog::flora
