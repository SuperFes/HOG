#pragma once

#include <cmath>
#include <numbers>

namespace hog::flora {

// sin and cos by one fixed polynomial: libm differs between platforms, and the same seed must
// grow the same tree everywhere. Good to about 1e-6 over the fold, plenty for growing.

inline float folded(float radians) noexcept {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float tau = 2.0F * pi;
    constexpr float half_pi = 0.5F * pi;
    float turn = radians - (tau * std::floor((radians / tau) + 0.5F));
    if (turn > half_pi) {
        turn = pi - turn;
    } else if (turn < -half_pi) {
        turn = -pi - turn;
    }
    return turn;
}

inline float turning_sin(float radians) noexcept {
    const float turn = folded(radians);
    const float square = turn * turn;
    return turn * (0.9999966F +
                   (square * (-0.16664824F + (square * (0.00830629F + (square * -0.00018363F))))));
}

inline float turning_cos(float radians) noexcept {
    return turning_sin(radians + (0.5F * std::numbers::pi_v<float>));
}

} // namespace hog::flora
