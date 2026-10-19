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

} // namespace hog::flora
