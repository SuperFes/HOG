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
    int grown = 2;            // levels grown, 1 to 3
    int cards = 3;            // foliage cards along each deepest branch, plus one at its tip
    float card_size = 1.5F;   // metres across a card
    float card_sway = 0.3F;   // ± fraction of card_size
    float card_start = 0.3F;  // of a branch before its cards begin
    float card_aspect = 1.0F; // a card's width as a fraction of its length
    float card_pitch = 0.5F;  // radians a card tips off its twig, give or take half
};

// The pine analogue: whorled boughs off a single trunk, a conical crown.
[[nodiscard]] Species pine();

} // namespace hog::flora
