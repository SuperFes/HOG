#include <hog/flora/species.hpp>

#include <algorithm>

namespace hog::flora {

namespace {

float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

// A factor on the species' prime: `young` for a sapling, `old` at the end, exactly 1 in its prime.
float across(float age, float young, float old) noexcept {
    if (age >= prime_age) {
        return mixed(1.0F, old, (age - prime_age) / (1.0F - prime_age));
    }
    return mixed(young, 1.0F, age / prime_age);
}

// Height for age: quick to rise while young, then barely taller, as stand.cpp grows them.
float rising(float age) noexcept {
    return 0.2F + (age * (2.0F - age));
}

} // namespace

Species pine() {
    Species pine;
    pine.grown = 2;
    pine.levels[0] = {.length = 11.0F,
                      .sway = 0.35F,
                      .radius = 0.26F,
                      .taper = 0.85F,
                      .sections = 10,
                      .sides = 6,
                      .children = 64,
                      .whorl = 4,
                      .start = 0.18F,
                      .angle = 1.45F,
                      .fall = 0.68F,
                      .gnarl = 0.015F,
                      .rise = 0.004F};
    pine.levels[1] = {.length = 2.4F,
                      .sway = 0.25F,
                      .radius = 0.38F,
                      .taper = 0.9F,
                      .sections = 3,
                      .sides = 3,
                      .children = 0,
                      .whorl = 1,
                      .start = 0.3F,
                      .angle = 0.8F,
                      .fall = 0.0F,
                      .gnarl = 0.05F,
                      .rise = -0.012F};
    pine.frond_width = 0.28F;
    pine.frond_sway = 0.25F;
    pine.frond_tilt = 0.55F;
    return pine;
}

Species aged(const Species& species, float age) noexcept {
    const float at = std::clamp(age, 0.0F, 1.0F);
    Species grown = species;
    Level& trunk = grown.levels[0];
    Level& boughs = grown.levels[1];
    trunk.length *= rising(at) / rising(prime_age);
    trunk.radius *= across(at, 0.25F, 1.35F);
    const int whorl = std::max(trunk.whorl, 1);
    const float whorls =
        static_cast<float>(trunk.children) * across(at, 0.75F, 1.0F) / static_cast<float>(whorl);
    trunk.children = whorl * std::max(static_cast<int>(whorls + 0.5F), 3);
    trunk.start *= across(at, 0.25F, 2.4F);
    trunk.fall *= across(at, 1.0F, 0.55F);
    trunk.angle *= across(at, 0.85F, 1.05F);
    trunk.gnarl *= across(at, 0.6F, 2.0F);
    boughs.length *= across(at, 0.5F, 1.5F);
    boughs.rise *= across(at, 0.0F, 1.6F);
    return grown;
}

} // namespace hog::flora
