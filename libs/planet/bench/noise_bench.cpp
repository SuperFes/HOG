#include <hog/planet/noise.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("noise throughput", "[benchmark]") {
    constexpr std::int64_t samples = 10'000;
    constexpr std::uint64_t seed = 1;

    BENCHMARK("perlin, 10k samples") {
        std::int64_t sum = 0;
        for (std::int64_t i = 0; i < samples; ++i) {
            sum += hog::planet::perlin(seed, {.x = i * 4'099, .y = i * 977, .z = -i * 1'531});
        }
        return sum;
    };

    BENCHMARK("fbm 8 octaves, 10k samples") {
        std::int64_t sum = 0;
        for (std::int64_t i = 0; i < samples; ++i) {
            sum += hog::planet::fbm(seed, {.x = i * 4'099, .y = i * 977, .z = -i * 1'531}, 8);
        }
        return sum;
    };
}
