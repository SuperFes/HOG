#pragma once

#include <cstdint>

namespace hog::flora {

// Deterministic on every platform: integers only, floats made from fixed bit counts.
class Dice {
public:
    explicit Dice(std::uint32_t seed) noexcept : state_{(std::uint64_t{seed} << 1U) | 1U} {}

    std::uint32_t roll() noexcept {
        state_ += 0x9e37'79b9'7f4a'7c15ULL;
        std::uint64_t mixed = state_;
        mixed = (mixed ^ (mixed >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94d0'49bb'1331'11ebULL;
        return static_cast<std::uint32_t>(mixed ^ (mixed >> 31U));
    }

    // Uniform in [0, 1).
    float unit() noexcept { return static_cast<float>(roll() >> 8U) * 0x1p-24F; }

    // Uniform in ±swing.
    float around(float swing) noexcept { return swing * ((2.0F * unit()) - 1.0F); }

private:
    std::uint64_t state_;
};

} // namespace hog::flora
