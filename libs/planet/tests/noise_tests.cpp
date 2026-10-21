#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using hog::planet::fine_fraction_bits;
using hog::planet::fixed_one;
using hog::planet::NoisePoint;

using FinePoint = hog::planet::BasicNoisePoint<fine_fraction_bits>;
constexpr std::int64_t fine_one = hog::planet::fixed_unit<fine_fraction_bits>;
constexpr int fine_shift = fine_fraction_bits - hog::planet::fixed_fraction_bits;

namespace {

constexpr std::uint64_t seed = 0xC0FFEE;

NoisePoint random_point(std::mt19937_64& rng, std::int64_t span) {
    std::uniform_int_distribution<std::int64_t> coordinate{-span, span};
    return {.x = coordinate(rng), .y = coordinate(rng), .z = coordinate(rng)};
}

// Identical on every platform, unlike the standard distributions, so statistical checks see
// the same samples everywhere.
NoisePoint hashed_point(std::uint64_t index, std::int64_t span) {
    const auto coordinate = [&](std::uint64_t axis) {
        const auto range = static_cast<std::uint64_t>(2 * span) + 1;
        return static_cast<std::int64_t>(hog::hash_values(index, axis) % range) - span;
    };
    return {.x = coordinate(0), .y = coordinate(1), .z = coordinate(2)};
}

FinePoint finer(NoisePoint point) {
    return {.x = point.x << fine_shift, .y = point.y << fine_shift, .z = point.z << fine_shift};
}

// The same algorithm in double precision, to measure the fixed-point rounding error.
template <int Bits>
double reference_perlin(std::uint64_t noise_seed, hog::planet::BasicNoisePoint<Bits> point) {
    const auto fade = [](double t) { return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0); };
    const auto lerp = [](double a, double b, double t) { return a + ((b - a) * t); };
    const auto cell = [](std::int64_t v) { return v >> Bits; };
    const auto fraction = [](std::int64_t v) {
        return hog::planet::fixed_to_double<Bits>(v & (hog::planet::fixed_unit<Bits> - 1));
    };
    const double fx = fraction(point.x);
    const double fy = fraction(point.y);
    const double fz = fraction(point.z);
    const auto corner = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        const auto h = static_cast<unsigned>(
            hog::planet::lattice_hash(noise_seed, cell(point.x) + dx, cell(point.y) + dy,
                                      cell(point.z) + dz) >>
            60);
        const double x = fx - static_cast<double>(dx);
        const double y = fy - static_cast<double>(dy);
        const double z = fz - static_cast<double>(dz);
        const double u = h < 8 ? x : y;
        double v = z;
        if (h < 4) {
            v = y;
        } else if (h == 12 || h == 14) {
            v = x;
        }
        return ((h & 1U) != 0 ? -u : u) + ((h & 2U) != 0 ? -v : v);
    };
    const double u = fade(fx);
    const double v = fade(fy);
    const double w = fade(fz);
    const double value = lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u),
                                   lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                              lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u),
                                   lerp(corner(0, 1, 1), corner(1, 1, 1), u), v),
                              w);
    return std::clamp(value, -1.0, 1.0);
}

} // namespace

// Evaluable at compile time, which also proves it's free of undefined behaviour there.
static_assert(hog::planet::perlin(seed, {.x = 3 * fixed_one, .y = -7 * fixed_one, .z = 0}) == 0);
static_assert(hog::planet::fbm(seed, {.x = 12'345, .y = -678, .z = 9}, 8) != 0);
static_assert(hog::planet::perlin_gradient(seed, {.x = 12'345, .y = -678, .z = 9}).dx != 0);
static_assert(hog::planet::ridges(seed, {.x = 12'345, .y = -678, .z = 9}, 8) != 0);
// At 24 bits too, from the fade's ends to coordinates as far out as noise points may reach.
static_assert(hog::planet::detail::fade<fine_fraction_bits>(fine_one - 1) <=
              std::int64_t{1} << hog::planet::detail::weight_bits);
static_assert(hog::planet::fbm(seed, FinePoint{.x = (fine_one << 22) - 1, .y = -fine_one, .z = 9},
                               hog::planet::max_octaves) != 0);
static_assert(hog::planet::ridges(seed, FinePoint{.x = (fine_one << 22) - 1, .y = 5, .z = -9},
                                  hog::planet::max_octaves, 4 * fine_one) != 0);

TEST_CASE("the integer cube root is the largest whose cube fits", "[planet][noise]") {
    using hog::planet::cube_root;
    CHECK(cube_root(0) == 0);
    CHECK(cube_root(1) == 1);
    CHECK(cube_root(7) == 1);
    CHECK(cube_root(8) == 2);
    for (std::uint64_t root = 2; root <= 2'642'245; root += 997) {
        const std::uint64_t cube = root * root * root;
        REQUIRE(cube_root(cube) == root);
        REQUIRE(cube_root(cube - 1) == root - 1);
    }
    // The largest cube a 64-bit value holds, and the top of the range.
    CHECK(cube_root(std::uint64_t{2'642'245} * 2'642'245 * 2'642'245) == 2'642'245);
    CHECK(cube_root(~std::uint64_t{0}) == 2'642'245);
}

TEST_CASE("fixed-point Perlin stays within two steps of the exact value", "[planet][noise]") {
    std::mt19937_64 rng{4};
    for (int sample = 0; sample < 100'000; ++sample) {
        const NoisePoint point = random_point(rng, 100 * fixed_one);
        const double exact = reference_perlin(seed, point) * static_cast<double>(fixed_one);
        REQUIRE(std::abs(exact - static_cast<double>(hog::planet::perlin(seed, point))) <= 2.0);
    }
}

TEST_CASE("Perlin noise is zero on the lattice", "[planet][noise]") {
    for (std::int64_t x = -3; x <= 3; ++x) {
        for (std::int64_t y = -3; y <= 3; ++y) {
            const NoisePoint lattice{.x = x * fixed_one, .y = y * fixed_one, .z = 5 * fixed_one};
            CHECK(hog::planet::perlin(seed, lattice) == 0);
        }
    }
}

TEST_CASE("Perlin noise stays within -1 and 1", "[planet][noise]") {
    std::mt19937_64 rng{1};
    std::int64_t lowest = 0;
    std::int64_t highest = 0;
    double sum = 0.0;
    constexpr int samples = 200'000;
    for (int sample = 0; sample < samples; ++sample) {
        const std::int64_t value = hog::planet::perlin(seed, random_point(rng, 1'000 * fixed_one));
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
        sum += hog::planet::fixed_to_double(value);
    }
    CHECK(lowest >= -fixed_one);
    CHECK(highest <= fixed_one);
    // It actually uses the range, and is centred.
    CHECK(lowest < -fixed_one / 2);
    CHECK(highest > fixed_one / 2);
    CHECK(std::abs(sum / samples) < 0.01);
}

TEST_CASE("Perlin noise is continuous, across lattice cells and zero too", "[planet][noise]") {
    std::mt19937_64 rng{2};
    for (int sample = 0; sample < 50'000; ++sample) {
        NoisePoint point = random_point(rng, 50 * fixed_one);
        if (sample % 3 == 0) {
            point.x = (point.x >> 16) << 16; // on a cell boundary
        }
        const NoisePoint next{.x = point.x + 1, .y = point.y, .z = point.z};
        // The slope is at most about 3.2, plus rounding, so one step moves the value by a few.
        const std::int64_t step =
            hog::planet::perlin(seed, next) - hog::planet::perlin(seed, point);
        REQUIRE(std::abs(step) <= 8);
    }
}

TEST_CASE("noise depends on the seed and is repeatable", "[planet][noise]") {
    const NoisePoint point{.x = 98'765, .y = -43'210, .z = 13'579};
    CHECK(hog::planet::perlin(seed, point) == hog::planet::perlin(seed, point));
    CHECK(hog::planet::perlin(seed, point) != hog::planet::perlin(seed + 1, point));
    CHECK(hog::planet::lattice_hash(seed, 1, 2, 3) != hog::planet::lattice_hash(seed, 1, 3, 2));
    CHECK(hog::planet::lattice_hash(seed, 1, 2, 3) != hog::planet::lattice_hash(seed + 1, 1, 2, 3));
}

TEST_CASE("lattice values cover [0, 1) evenly", "[planet][noise]") {
    std::array<int, 4> quarters{};
    for (std::int64_t x = 0; x < 100; ++x) {
        for (std::int64_t y = -50; y < 50; ++y) {
            const std::int64_t value = hog::planet::lattice_value(seed, x, y, 7);
            REQUIRE(value >= 0);
            REQUIRE(value < fixed_one);
            ++quarters.at(static_cast<std::size_t>(value / (fixed_one / 4)));
        }
    }
    for (const int count : quarters) {
        CHECK(count > 2'250);
        CHECK(count < 2'750);
    }
}

TEST_CASE("fractal noise adds octaves and stays normalised", "[planet][noise]") {
    const NoisePoint point{.x = 77'777, .y = 123'456, .z = -98'765};
    // One octave is plain Perlin with the first octave's seed.
    CHECK(hog::planet::fbm(seed, point, 1) ==
          hog::planet::perlin(hog::hash_combine(seed, 0), point));
    CHECK(hog::planet::fbm(seed, point, 6) != hog::planet::fbm(seed, point, 5));

    std::mt19937_64 rng{3};
    for (int sample = 0; sample < 20'000; ++sample) {
        const std::int64_t value =
            hog::planet::fbm(seed, random_point(rng, 10'000 * fixed_one), hog::planet::max_octaves);
        REQUIRE(value >= -fixed_one);
        REQUIRE(value <= fixed_one);
    }
}

TEST_CASE("noise output is bit-identical on every platform", "[planet][noise]") {
    // Golden value: changing it changes every generated planet.
    std::uint64_t hash = 0;
    for (std::int64_t x = -20; x < 20; ++x) {
        for (std::int64_t y = -20; y < 20; ++y) {
            const NoisePoint point{.x = x * 23'456, .y = y * 34'567, .z = (x * y) * 1'234};
            hash = hog::hash_values(hash, hog::planet::perlin(seed, point),
                                    hog::planet::fbm(seed, point, 8));
        }
    }
    CHECK(hash == 0x56A1'9C08'0A36'E71F);
}

TEST_CASE("the gradient's value is plain Perlin", "[planet][noise]") {
    std::mt19937_64 rng{5};
    for (int sample = 0; sample < 100'000; ++sample) {
        const NoisePoint point = random_point(rng, 1'000 * fixed_one);
        REQUIRE(hog::planet::perlin_gradient(seed, point).value ==
                hog::planet::perlin(seed, point));
    }
}

TEST_CASE("the gradient matches finite differences", "[planet][noise]") {
    // A sixty-fourth of a lattice cell: small enough for the curvature, large enough that
    // the value's rounding stays below the tolerance.
    constexpr std::int64_t step = fixed_one / 64;
    constexpr std::int64_t tolerance = fixed_one / 100;
    const auto difference = [](NoisePoint low, NoisePoint high) {
        return ((hog::planet::perlin(seed, high) - hog::planet::perlin(seed, low)) * fixed_one) /
               (2 * step);
    };
    std::mt19937_64 rng{6};
    for (int sample = 0; sample < 20'000; ++sample) {
        const NoisePoint p = random_point(rng, 1'000 * fixed_one);
        const auto gradient = hog::planet::perlin_gradient(seed, p);
        if (std::abs(gradient.value) > fixed_one * 9 / 10) {
            continue; // near the clamp
        }
        REQUIRE(std::abs(difference({p.x - step, p.y, p.z}, {p.x + step, p.y, p.z}) - gradient.dx) <
                tolerance);
        REQUIRE(std::abs(difference({p.x, p.y - step, p.z}, {p.x, p.y + step, p.z}) - gradient.dy) <
                tolerance);
        REQUIRE(std::abs(difference({p.x, p.y, p.z - step}, {p.x, p.y, p.z + step}) - gradient.dz) <
                tolerance);
    }
}

TEST_CASE("ridges stay within 0 and 1, and erosion only lowers them", "[planet][noise]") {
    std::mt19937_64 rng{7};
    std::int64_t lowest = fixed_one;
    std::int64_t highest = 0;
    for (int sample = 0; sample < 20'000; ++sample) {
        const NoisePoint point = random_point(rng, 1'000 * fixed_one);
        const std::int64_t eroded = hog::planet::ridges(seed, point, 8);
        const std::int64_t plain = hog::planet::ridges(seed, point, 8, 0);
        REQUIRE(eroded >= 0);
        REQUIRE(plain <= fixed_one);
        REQUIRE(eroded <= plain);
        lowest = std::min(lowest, eroded);
        highest = std::max(highest, plain);
    }
    CHECK(lowest < fixed_one / 4);
    CHECK(highest > fixed_one * 3 / 4);
    // One octave is undamped: there is no slope before it.
    const NoisePoint point{.x = 77'777, .y = 123'456, .z = -98'765};
    CHECK(hog::planet::ridges(seed, point, 1) == hog::planet::ridges(seed, point, 1, 0));
}

TEST_CASE("ridges are continuous, erosion included", "[planet][noise]") {
    std::mt19937_64 rng{9};
    for (int sample = 0; sample < 50'000; ++sample) {
        const NoisePoint point = random_point(rng, 100 * fixed_one);
        const NoisePoint next{.x = point.x + 1, .y = point.y, .z = point.z};
        const std::int64_t step =
            hog::planet::ridges(seed, next, 8) - hog::planet::ridges(seed, point, 8);
        REQUIRE(std::abs(step) <= 16);
    }
}

TEST_CASE("erosion smooths steep ground more than flat ground", "[planet][noise]") {
    // Fine detail as the second difference over a short step; steepness from the plain ridges
    // over a long one.
    constexpr std::int64_t fine = fixed_one / 256;
    constexpr std::int64_t coarse = fixed_one / 16;
    const auto detail = [](NoisePoint p, std::int64_t erosion) {
        const auto at = [&](std::int64_t dx) {
            return hog::planet::ridges(seed, {.x = p.x + dx, .y = p.y, .z = p.z}, 8, erosion);
        };
        return std::abs(at(fine) + at(-fine) - (2 * at(0)));
    };
    struct Sample {
        std::int64_t steepness;
        std::int64_t plain;
        std::int64_t eroded;
    };
    std::vector<Sample> samples;
    for (std::uint64_t sample = 0; sample < 8'000; ++sample) {
        const NoisePoint p = hashed_point(sample, 1'000 * fixed_one);
        const auto plain_at = [&](std::int64_t dx) {
            return hog::planet::ridges(seed, {.x = p.x + dx, .y = p.y, .z = p.z}, 8, 0);
        };
        samples.push_back({.steepness = std::abs(plain_at(coarse) - plain_at(-coarse)),
                           .plain = detail(p, 0),
                           .eroded = detail(p, 4 * fixed_one)});
    }
    std::ranges::stable_sort(samples, {}, &Sample::steepness);
    const auto mean_detail = [&](std::size_t begin, std::size_t end, auto member) {
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += static_cast<double>(samples[i].*member);
        }
        return sum / static_cast<double>(end - begin);
    };
    const std::size_t quarter = samples.size() / 4;
    const double plain_ratio =
        mean_detail(samples.size() - quarter, samples.size(), &Sample::plain) /
        mean_detail(0, quarter, &Sample::plain);
    const double eroded_ratio =
        mean_detail(samples.size() - quarter, samples.size(), &Sample::eroded) /
        mean_detail(0, quarter, &Sample::eroded);
    CHECK(eroded_ratio < plain_ratio * 0.9);
}

TEST_CASE("fractal noise checks the octave count", "[planet][noise]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(hog::planet::fbm(seed, {}, 0), hog::testing::AssertionFailed);
    CHECK_THROWS_AS(hog::planet::fbm(seed, {}, hog::planet::max_octaves + 1),
                    hog::testing::AssertionFailed);
    CHECK_THROWS_AS(hog::planet::ridges(seed, {}, 0), hog::testing::AssertionFailed);
    CHECK_THROWS_AS(hog::planet::ridges(seed, {}, 4, -1), hog::testing::AssertionFailed);
}

TEST_CASE("fine Perlin stays within two of its steps of the exact value", "[planet][noise]") {
    std::mt19937_64 rng{10};
    for (int sample = 0; sample < 100'000; ++sample) {
        const FinePoint point = finer(random_point(rng, 100 * fixed_one));
        const FinePoint between{.x = point.x + (sample % 251), .y = point.y + 77, .z = point.z};
        const double exact = reference_perlin(seed, between) * static_cast<double>(fine_one);
        REQUIRE(std::abs(exact - static_cast<double>(hog::planet::perlin(seed, between))) <= 2.0);
    }
}

TEST_CASE("fine noise is coarse noise with more precision", "[planet][noise]") {
    const auto coarser = [](std::int64_t fine) { return fine >> fine_shift; };
    std::mt19937_64 rng{11};
    for (int sample = 0; sample < 20'000; ++sample) {
        const NoisePoint point = random_point(rng, 1'000 * fixed_one);
        REQUIRE(std::abs(coarser(hog::planet::perlin(seed, finer(point))) -
                         hog::planet::perlin(seed, point)) <= 2);
        REQUIRE(std::abs(coarser(hog::planet::fbm(seed, finer(point), 8)) -
                         hog::planet::fbm(seed, point, 8)) <= 4);
        REQUIRE(std::abs(coarser(hog::planet::ridges(seed, finer(point), 8, 2 * fine_one)) -
                         hog::planet::ridges(seed, point, 8, 2 * fixed_one)) <= 16);
    }
}

TEST_CASE("fine noise moves between the points coarse noise steps by", "[planet][noise]") {
    std::mt19937_64 rng{12};
    for (int sample = 0; sample < 2'000; ++sample) {
        const FinePoint start = finer(random_point(rng, 100 * fixed_one));
        // Within a step of coarse noise, fine ridges change by a few of their own steps at a time
        // and take many values, rather than holding one and jumping.
        std::int64_t previous = hog::planet::ridges(seed, start, 6, 2 * fine_one);
        std::vector<std::int64_t> values{previous};
        for (std::int64_t step = 1; step <= std::int64_t{1} << fine_shift; ++step) {
            const std::int64_t value = hog::planet::ridges(
                seed, FinePoint{.x = start.x + step, .y = start.y, .z = start.z}, 6, 2 * fine_one);
            REQUIRE(std::abs(value - previous) <= 16);
            previous = value;
            values.push_back(value);
        }
        std::ranges::sort(values);
        const auto distinct =
            std::ranges::distance(values.begin(), std::ranges::unique(values).begin());
        const std::int64_t spread = values.back() - values.front();
        CHECK(distinct >= std::min<std::int64_t>(spread / 4, 64));
    }
}
