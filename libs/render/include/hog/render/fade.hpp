#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace hog::render {

// Seconds a mesh takes to fade in or out as another takes its place.
inline constexpr double fade_seconds = 0.5;
// Steps a fade is drawn in.
inline constexpr std::uint32_t fade_steps = 255;

// A mesh fading in or out a pixel at a time: the pixels one fading in is drawn in are the very ones
// one fading out over the same seconds gives up, so the two together cover each pixel once.
struct Fade {
    bool in = true;
    std::optional<double> began; // the seconds it began at, from the first frame it's drawn in

    // How far it's come, from 0 to 1, at `seconds`; none before it's begun.
    [[nodiscard]] double along(double seconds) const noexcept {
        return began ? std::clamp((seconds - *began) / fade_seconds, 0.0, 1.0) : 0.0;
    }
    [[nodiscard]] bool done(double seconds) const noexcept { return along(seconds) >= 1.0; }
    // What a draw of it carries as its first instance, as the blocks' shader reads it: the steps
    // come, plus one, and past them as many again fading out.
    [[nodiscard]] std::uint32_t instance(double seconds) const noexcept {
        const auto steps =
            static_cast<std::uint32_t>(std::lround(along(seconds) * double{fade_steps}));
        return 1 + steps + (in ? 0 : fade_steps + 1);
    }
};

} // namespace hog::render
