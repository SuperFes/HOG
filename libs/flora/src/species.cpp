#include <hog/flora/species.hpp>

#include <algorithm>
#include <cmath>

namespace hog::flora {

namespace {

float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

// The span's factor at `age`: exactly 1 in its prime.
float across(float age, Span span) noexcept {
    if (age >= prime_age) {
        return mixed(1.0F, span.old, (age - prime_age) / (1.0F - prime_age));
    }
    return mixed(span.young, 1.0F, age / prime_age);
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
    // A sapling is a short, thin, upswept cone of a few whorls down to the ground; an old pine
    // has a stout trunk, its lower boughs shed, its crown raised and rounded, its boughs drooping.
    pine.ageing = {.girth = {.young = 0.25F, .old = 1.35F},
                   .crowded = {.young = 0.75F, .old = 1.0F},
                   .raised = {.young = 0.25F, .old = 2.4F},
                   .fall = {.young = 1.0F, .old = 0.55F},
                   .lean = {.young = 0.85F, .old = 1.05F},
                   .gnarl = {.young = 0.6F, .old = 2.0F},
                   .reach = {.young = 0.5F, .old = 1.5F},
                   .rise = {.young = 0.0F, .old = 1.6F}};
    return pine;
}

Species oak() {
    Species oak;
    oak.grown = 3;
    oak.levels[0] = {.length = 8.0F,
                     .sway = 0.2F,
                     .radius = 0.42F,
                     .taper = 0.75F,
                     .sections = 8,
                     .sides = 7,
                     .children = 7,
                     .whorl = 1,
                     .start = 0.35F,
                     .angle = 0.9F,
                     .fall = 0.25F,
                     .gnarl = 0.09F,
                     .rise = 0.0F};
    oak.levels[1] = {.length = 6.5F,
                     .sway = 0.3F,
                     .radius = 0.6F,
                     .taper = 0.8F,
                     .sections = 5,
                     .sides = 5,
                     .children = 16,
                     .whorl = 1,
                     .start = 0.1F,
                     .angle = 0.75F,
                     .fall = 0.4F,
                     .gnarl = 0.18F,
                     .rise = 0.012F};
    oak.levels[2] = {.length = 1.8F,
                     .sway = 0.3F,
                     .radius = 0.5F,
                     .taper = 0.9F,
                     .sections = 3,
                     .sides = 3,
                     .children = 0,
                     .whorl = 1,
                     .start = 0.3F,
                     .angle = 0.8F,
                     .fall = 0.0F,
                     .gnarl = 0.2F,
                     .rise = 0.0F};
    oak.frond_width = 0.6F;
    oak.frond_sway = 0.3F;
    oak.frond_tilt = 0.7F;
    oak.frond_fullness = 1.0F;
    oak.woody = 2;
    // A sapling is a thin whip of a few upswept limbs; an old oak a massive trunk under limbs
    // spread wide and level, gnarled with age.
    oak.ageing = {.girth = {.young = 0.2F, .old = 1.5F},
                  .crowded = {.young = 0.5F, .old = 1.0F},
                  .raised = {.young = 0.6F, .old = 1.3F},
                  .fall = {.young = 1.0F, .old = 1.0F},
                  .lean = {.young = 0.6F, .old = 1.0F},
                  .gnarl = {.young = 0.5F, .old = 1.8F},
                  .reach = {.young = 0.4F, .old = 1.2F},
                  .rise = {.young = 2.0F, .old = 1.0F}};
    // At 45 degrees north: turning by mid-October, bare by mid-November, out again in May.
    oak.leafing = {.turn = 11.4F,
                   .fall = 1.8F,
                   .flush = 0.85F,
                   .spread = 0.6F,
                   .first = {0.15F, 0.105F, 0.02F},
                   .last = {0.11F, 0.04F, 0.012F}};
    return oak;
}

Species maple() {
    Species maple;
    maple.grown = 3;
    maple.levels[0] = {.length = 10.0F,
                       .sway = 0.15F,
                       .radius = 0.36F,
                       .taper = 0.8F,
                       .sections = 8,
                       .sides = 7,
                       .children = 9,
                       .whorl = 1,
                       .start = 0.3F,
                       .angle = 0.85F,
                       .fall = 0.45F,
                       .gnarl = 0.04F,
                       .rise = 0.0F};
    maple.levels[1] = {.length = 6.0F,
                       .sway = 0.25F,
                       .radius = 0.55F,
                       .taper = 0.8F,
                       .sections = 5,
                       .sides = 5,
                       .children = 15,
                       .whorl = 1,
                       .start = 0.1F,
                       .angle = 0.7F,
                       .fall = 0.3F,
                       .gnarl = 0.1F,
                       .rise = 0.015F};
    maple.levels[2] = {.length = 1.6F,
                       .sway = 0.3F,
                       .radius = 0.5F,
                       .taper = 0.9F,
                       .sections = 3,
                       .sides = 3,
                       .children = 0,
                       .whorl = 1,
                       .start = 0.3F,
                       .angle = 0.8F,
                       .fall = 0.0F,
                       .gnarl = 0.15F,
                       .rise = 0.0F};
    maple.frond_width = 0.7F;
    maple.frond_sway = 0.25F;
    maple.frond_tilt = 0.6F;
    maple.frond_fullness = 1.0F;
    maple.woody = 2;
    // A sapling is a slim upright whip; an old maple a stout trunk under a crown grown broader.
    maple.ageing = {.girth = {.young = 0.25F, .old = 1.35F},
                    .crowded = {.young = 0.5F, .old = 1.0F},
                    .raised = {.young = 0.5F, .old = 1.2F},
                    .fall = {.young = 1.0F, .old = 0.8F},
                    .lean = {.young = 0.7F, .old = 1.1F},
                    .gnarl = {.young = 0.5F, .old = 1.5F},
                    .reach = {.young = 0.45F, .old = 1.15F},
                    .rise = {.young = 1.5F, .old = 0.8F}};
    // At 45 degrees north: orange by early October, scarlet and thinning by its third week,
    // bare by November; out a little before the oaks in spring.
    maple.leafing = {.turn = 11.7F,
                     .fall = 1.4F,
                     .flush = 0.8F,
                     .spread = 0.5F,
                     .first = {0.32F, 0.14F, 0.012F},
                     .last = {0.26F, 0.025F, 0.015F}};
    return maple;
}

Species palm() {
    Species palm;
    palm.grown = 2;
    palm.levels[0] = {.length = 15.0F,
                      .sway = 0.2F,
                      .radius = 0.2F,
                      .taper = 0.3F,
                      .sections = 12,
                      .sides = 7,
                      .children = 21,
                      .whorl = 7,
                      .start = 0.96F,
                      .angle = 1.15F,
                      .fall = 0.0F,
                      .gnarl = 0.02F,
                      .rise = 0.004F};
    palm.levels[1] = {.length = 5.0F,
                      .sway = 0.2F,
                      .radius = 0.4F,
                      .taper = 0.9F,
                      .sections = 6,
                      .sides = 3,
                      .children = 0,
                      .whorl = 1,
                      .start = 0.3F,
                      .angle = 0.8F,
                      .fall = 0.0F,
                      .gnarl = 0.04F,
                      .rise = -0.012F};
    palm.frond_width = 0.17F;
    palm.frond_sway = 0.2F;
    palm.frond_tilt = 0.45F;
    palm.frond_fullness = 0.3F;
    palm.woody = 1;
    palm.lean = 0.3F;
    palm.fruit = {.count = 9,
                  .radius = 0.12F,
                  .length = 1.3F,
                  .bearing = {.young = 0.0F, .old = 0.6F},
                  .fallen = 5,
                  .scatter = 2.5F,
                  .lying = {.young = 0.0F, .old = 1.6F}};
    // A young palm is a short stem under a spray of short, upswept fronds and no fruit; an old one
    // stands its crown of drooping fronds on a trunk grown hardly thicker, bearing less.
    palm.ageing = {.girth = {.young = 0.75F, .old = 1.05F},
                   .crowded = {.young = 1.0F, .old = 1.0F},
                   .raised = {.young = 1.0F, .old = 1.0F},
                   .fall = {.young = 1.0F, .old = 1.0F},
                   .lean = {.young = 0.75F, .old = 1.1F},
                   .gnarl = {.young = 0.5F, .old = 1.5F},
                   .reach = {.young = 0.6F, .old = 1.0F},
                   .rise = {.young = 0.4F, .old = 1.3F}};
    return palm;
}

Species species_of(Genus genus) {
    switch (genus) {
    case Genus::Oak:
        return oak();
    case Genus::Maple:
        return maple();
    case Genus::Palm:
        return palm();
    case Genus::Pine:
        break;
    }
    return pine();
}

Species aged(const Species& species, float age) noexcept {
    const float at = std::clamp(age, 0.0F, 1.0F);
    Species grown = species;
    Level& trunk = grown.levels[0];
    Level& boughs = grown.levels[1];
    const Ageing& ageing = species.ageing;
    trunk.length *= rising(at) / rising(prime_age);
    trunk.radius *= across(at, ageing.girth);
    const int whorl = std::max(trunk.whorl, 1);
    const float whorls =
        static_cast<float>(trunk.children) * across(at, ageing.crowded) / static_cast<float>(whorl);
    trunk.children = whorl * std::max(static_cast<int>(std::lround(whorls)), 3);
    trunk.start *= across(at, ageing.raised);
    trunk.fall *= across(at, ageing.fall);
    trunk.angle *= across(at, ageing.lean);
    trunk.gnarl *= across(at, ageing.gnarl);
    boughs.length *= across(at, ageing.reach);
    boughs.rise *= across(at, ageing.rise);
    grown.fruit.count = static_cast<int>(
        std::lround(static_cast<float>(species.fruit.count) * across(at, species.fruit.bearing)));
    grown.fruit.fallen = static_cast<int>(
        std::lround(static_cast<float>(species.fruit.fallen) * across(at, species.fruit.lying)));
    return grown;
}

} // namespace hog::flora
