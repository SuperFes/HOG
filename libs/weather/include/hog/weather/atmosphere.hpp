#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>
#include <hog/weather/field.hpp>
#include <hog/weather/stencil.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>

namespace hog::weather {

// What a column of air shows from the ground and from afar. Heights are metres over the
// reference level.
struct Column {
    float cloud = 0.0F;    // mm of condensed water above
    float cover = 0.0F;    // share of the sky it covers, 0 to 1
    float falling = 0.0F;  // mm an hour reaching the ground
    float snow = 0.0F;     // share of it that is snow
    Vec2 drift{0.0F};      // (east, north) m/s the cloud is carried at
    Vec2 wind{0.0F};       // (east, north) m/s in the lowest open air
    float humidity = 0.0F; // of the lowest open air, 0 to 1
    bool sea = false;      // whether the sea lies under the air
    // The lowest and highest cloud; 0 where there is none. Cloud on the ground reaches a layer
    // under the air's lowest, into the hollows the column's ground is taken over.
    float base = 0.0F;
    float top = 0.0F;
};

// The air of a whole planet, stepped on in game time.
class Atmosphere {
public:
    // Longest stretch the air is stepped in at once, seconds.
    static constexpr double longest_step = 120.0;

    // The air over `generator`'s planet.
    Atmosphere(const globe::Generator& generator, int level, int layers);

    [[nodiscard]] const Field& field() const noexcept { return field_; }
    [[nodiscard]] Field& field() noexcept { return field_; }
    [[nodiscard]] const globe::Planet& planet() const noexcept { return planet_; }

    // Runs the air on over `seconds`, in steps of at most longest_step; `sun(elapsed)` gives the
    // unit direction to the sun that many seconds in.
    template <std::invocable<double> Sun>
    void advance(double seconds, Sun sun) {
        double done = 0.0;
        while (done < seconds) {
            const double step = std::min(longest_step, seconds - done);
            advance_once(sun(done + (0.5 * step)), step);
            done += step;
        }
    }

    [[nodiscard]] Column column(std::size_t column) const;

private:
    void advance_once(DVec3 sun, double seconds);

    globe::Planet planet_;
    Field field_;
    Stencil stencil_;
};

} // namespace hog::weather
