#include <hog/flora/species.hpp>

namespace hog::flora {

Species pine() {
    Species pine;
    pine.grown = 2;
    pine.levels[0] = {.length = 11.0F,
                      .sway = 0.35F,
                      .radius = 0.26F,
                      .taper = 0.85F,
                      .sections = 10,
                      .sides = 6,
                      .children = 28,
                      .whorl = 5,
                      .start = 0.22F,
                      .angle = 1.35F,
                      .fall = 0.78F,
                      .gnarl = 0.015F,
                      .rise = 0.004F};
    pine.levels[1] = {.length = 2.8F,
                      .sway = 0.25F,
                      .radius = 0.38F,
                      .taper = 0.9F,
                      .sections = 4,
                      .sides = 4,
                      .children = 0,
                      .whorl = 1,
                      .start = 0.3F,
                      .angle = 0.8F,
                      .fall = 0.0F,
                      .gnarl = 0.05F,
                      .rise = -0.006F};
    pine.cards = 6;
    pine.card_size = 2.2F;
    pine.card_sway = 0.35F;
    pine.card_start = 0.15F;
    pine.card_aspect = 0.5F;
    pine.card_pitch = 0.22F;
    return pine;
}

} // namespace hog::flora
