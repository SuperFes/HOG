#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::grid {

// Something hot: kelvin over the baseline at its middle, falling smoothly to none `radius`
// metres away. Places are metres from the planet's centre.
struct HeatSource {
    DVec3 place{0.0};
    double radius = 0.0;
    double kelvin = 0.0;

    friend constexpr bool operator==(const HeatSource&, const HeatSource&) noexcept = default;
};

// Where it's hotter than the baseline the planet gives (globe's baseline_temperature): only
// there is anything kept.
struct Heat {
    static constexpr std::uint8_t format = 1;

    std::vector<HeatSource> sources;

    // Kelvin over the baseline at `place`, of every source reaching it.
    [[nodiscard]] double over(DVec3 place) const noexcept;

    // Little-endian: format, a varint count, then each source's place, radius and kelvin as
    // doubles.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't sources, or a source without a positive radius.
    [[nodiscard]] static Result<Heat> decode(std::span<const std::byte> bytes);

    friend bool operator==(const Heat&, const Heat&) = default;
};

} // namespace hog::grid
