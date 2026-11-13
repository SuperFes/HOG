#include <hog/core/hash.hpp>
#include <hog/globe/noise.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <random>

using hog::DVec3;
using hog::globe::Noise;

namespace {

// Points scattered through a box a few hundred cells across, the same every run.
DVec3 scattered(std::uint64_t k) {
    const auto unit = [&](std::uint64_t axis) {
        return static_cast<double>(hog::hash_values(77, k, axis) >> 11U) * 0x1.0p-53;
    };
    return (DVec3{unit(0), unit(1), unit(2)} * 400.0) - 200.0;
}

} // namespace

TEST_CASE("gradient noise stays within about one and is zero on the lattice", "[globe][noise]") {
    double largest = 0.0;
    double squares = 0.0;
    constexpr int count = 20'000;
    for (std::uint64_t k = 0; k < count; ++k) {
        const double value = hog::globe::gradient_noise(scattered(k), 5).value;
        largest = std::max(largest, std::abs(value));
        squares += value * value;
    }
    CHECK(largest < 1.1);
    CHECK(largest > 0.6);
    const double spread = std::sqrt(squares / count);
    CHECK(spread > 0.2);
    CHECK(spread < 0.5);
    CHECK(hog::globe::gradient_noise(DVec3{3.0, -7.0, 12.0}, 5).value == 0.0);
}

TEST_CASE("the noise's gradient is its true slope", "[globe][noise]") {
    constexpr double h = 1e-6;
    const hog::globe::Octaves octaves{.wavelength = 50.0, .gain = 0.5, .finest = 0.5};
    for (std::uint64_t k = 0; k < 500; ++k) {
        const DVec3 at = scattered(k);
        const Noise here = hog::globe::fbm(at, octaves, 0.0, 9);
        const Noise single = hog::globe::gradient_noise(at * 0.1, 9);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            DVec3 step{0.0};
            step[axis] = h;
            const double slope = (hog::globe::fbm(at + step, octaves, 0.0, 9).value -
                                  hog::globe::fbm(at - step, octaves, 0.0, 9).value) /
                                 (2.0 * h);
            CHECK(std::abs(slope - here.gradient[axis]) < 1e-6);
            const double single_slope = (hog::globe::gradient_noise((at + step) * 0.1, 9).value -
                                         hog::globe::gradient_noise((at - step) * 0.1, 9).value) /
                                        (2.0 * h);
            CHECK(std::abs(single_slope - (0.1 * single.gradient[axis])) < 1e-6);
        }
    }
}

TEST_CASE("the noise favours no direction", "[globe][noise]") {
    // The mean squared slope along the axes, a face diagonal and a body diagonal agree: lattice
    // noise with axis-bound gradients would show its grid here.
    const std::array<DVec3, 4> ways{DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 0.0, 1.0},
                                    glm::normalize(DVec3{1.0, 1.0, 0.0}),
                                    glm::normalize(DVec3{1.0, 1.0, 1.0})};
    std::array<double, 4> slope{};
    constexpr int count = 40'000;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 gradient = hog::globe::gradient_noise(scattered(k), 3).gradient;
        for (std::size_t way = 0; way < ways.size(); ++way) {
            const double along = glm::dot(gradient, ways.at(way));
            slope.at(way) += along * along;
        }
    }
    const auto [low, high] = std::ranges::minmax(slope);
    CHECK(high / low < 1.05);
}

TEST_CASE("coarser samples drop the short octaves smoothly", "[globe][noise]") {
    const hog::globe::Octaves octaves{.wavelength = 1000.0, .gain = 0.5, .finest = 1.0};
    const DVec3 at{123.4, -56.7, 890.1};
    const double fine = hog::globe::fbm(at, octaves, 0.0, 4).value;
    const double same = hog::globe::fbm(at, octaves, 0.5, 4).value;
    CHECK(fine == same);
    // Each step of detail changes the value less than the octaves it drops can reach.
    double last = fine;
    for (const double lod : {2.0, 8.0, 32.0, 128.0}) {
        const double coarse = hog::globe::fbm(at, octaves, lod, 4).value;
        CHECK(std::abs(coarse - last) < 4.0 * lod / 1000.0 * 1.1);
        last = coarse;
    }
    // And a tiny change of detail changes the value only a little: no pops.
    CHECK(std::abs(hog::globe::fbm(at, octaves, 10.0, 4).value -
                   hog::globe::fbm(at, octaves, 10.001, 4).value) < 1e-3);
}

TEST_CASE("the same seed gives the same noise, bit for bit", "[globe][noise]") {
    const hog::globe::Octaves octaves{.wavelength = 2000.0, .gain = 0.5, .finest = 0.5};
    std::uint64_t digest = 0;
    for (std::uint64_t k = 0; k < 2'000; ++k) {
        const Noise sample = hog::globe::fbm(scattered(k) * 300.0, octaves, 0.0, 11);
        for (const double value :
             {sample.value, sample.gradient.x, sample.gradient.y, sample.gradient.z}) {
            digest = hog::hash_combine(digest, std::bit_cast<std::uint64_t>(value));
        }
    }
    CHECK(hog::globe::fbm(DVec3{1.0, 2.0, 3.0}, octaves, 0.0, 12).value !=
          hog::globe::fbm(DVec3{1.0, 2.0, 3.0}, octaves, 0.0, 11).value);
    // If this moves, the world is generated differently than before, or on another platform.
    CHECK(digest == 0x8fa4'ad26'7832'9735ULL);
}

TEST_CASE("fbm stays within its bounds over a ball", "[globe][noise]") {
    const hog::globe::Octaves octaves{.wavelength = 4000.0, .gain = 0.5, .finest = 5.0};
    std::mt19937_64 random{11};
    std::uniform_real_distribution<double> unit{-1.0, 1.0};
    for (const double reach : {1.0, 30.0, 400.0, 5000.0}) {
        for (const double lod : {0.0, 20.0}) {
            double widest = 0.0;
            double sampled = 0.0;
            for (int ball = 0; ball < 20; ++ball) {
                const hog::DVec3 middle =
                    hog::DVec3{unit(random), unit(random), unit(random)} * 6.0e6;
                const hog::globe::Interval bounds =
                    hog::globe::fbm_bounds(middle, reach, octaves, lod, 9);
                double low = 1e300;
                double high = -1e300;
                for (int n = 0; n < 400; ++n) {
                    const hog::DVec3 off{unit(random), unit(random), unit(random)};
                    if (glm::length(off) > 1.0) {
                        continue;
                    }
                    const double value =
                        hog::globe::fbm(middle + (off * reach), octaves, lod, 9).value;
                    CHECK(bounds.contains(value));
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                widest += bounds.high - bounds.low;
                sampled += high - low;
            }
            WARN("reach " << reach << " m, lod " << lod << ": sampled span " << sampled / widest
                          << " of the bounds'");
        }
    }
}
