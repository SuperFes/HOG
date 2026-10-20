#pragma once

#include <hog/flora/season.hpp>

#include <array>
#include <cstdint>

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

// A factor on a species in its prime: what it scales by for a sapling and for an old tree, 1 in
// between at prime_age.
struct Span {
    float young = 1.0F;
    float old = 1.0F;
};

// How a species changes with age, as factors on its prime's trunk and the level off it.
struct Ageing {
    Span girth;   // the trunk's radius
    Span crowded; // branches off the trunk, never under three whorls
    Span raised;  // where they begin up it
    Span fall;    // how much shorter toward its tip
    Span lean;    // how far they lean off it
    Span gnarl;   // how much the trunk wanders
    Span reach;   // the branches' length
    Span rise;    // their pull toward upright, or droop
};

// Fruit hung in a ring just under where the trunk's crown begins, each an ellipsoid of wood.
struct Fruit {
    int count = 0;       // in its prime
    float radius = 0.1F; // metres across its middle
    float length = 1.4F; // of its width, end to end
    Span bearing;        // the count's factor with age
};

// A species is a small rule set the grammar grows an individual from, deterministically from a
// seed: the same on every machine, as many times as it's grown.
struct Species {
    std::array<Level, 3> levels;
    int grown = 2;             // levels grown, 1 to 3
    float frond_width = 0.25F; // a frond's base half-width, as a fraction of its twig's length
    float frond_sway = 0.25F;  // ± fraction of frond_width each frond differs by
    float frond_tilt = 0.5F;   // radians each of a frond's two blades rolls off flat
    // 0 thins a frond evenly to its tip, as needles; 1 holds it full and rounds it off, as leaves.
    float frond_fullness = 0.0F;
    int woody = 1; // levels meshed as wood; the fronds cover the twigs past them
    // Radians the trunk leans off upright at its foot, at most, its own way for each tree; its
    // rise curves it back.
    float lean = 0.0F;
    Fruit fruit;
    Ageing ageing;
    Leafing leafing; // evergreen unless set
};

// The kinds of tree grown, each its own species.
enum class Genus : std::uint8_t { Pine, Oak, Maple, Palm };
inline constexpr int genus_count = 4;

// The pine analogue: whorled boughs off a single trunk, a conical crown.
[[nodiscard]] Species pine();
// The oak analogue: a short trunk forking into gnarled limbs, a broad rounded crown of leaves.
[[nodiscard]] Species oak();
// The maple analogue: a straight trunk carried up into steeply climbing limbs, a dense oval crown
// that turns early and bright.
[[nodiscard]] Species maple();
// The palm analogue: a bare trunk leaning from its foot and curving back up, a crown of long
// arching fronds, coconuts hung beneath it.
[[nodiscard]] Species palm();
[[nodiscard]] Species species_of(Genus genus);

// The age a species' rules describe it at, in its prime; 0 is a sapling, 1 old.
inline constexpr float prime_age = 0.7F;

// The species as it grows at `age`, 0 to 1, by its own ageing; its height rises quickly while
// young, then barely more. Exactly the species at prime_age; pure arithmetic, so as bit-stable as
// grow().
[[nodiscard]] Species aged(const Species& species, float age) noexcept;

} // namespace hog::flora
