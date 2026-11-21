#include "../src/formed.hpp"
#include "../src/ground_cells.hpp"
#include "../src/terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;

namespace {

namespace terms = hog::globe::terms;

double unit(std::uint64_t bits) {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

DVec3 somewhere(std::uint64_t n) {
    const double z = (2.0 * unit(hog::hash_values(29, n, 0))) - 1.0;
    const double turn = 2.0 * std::numbers::pi * unit(hog::hash_values(29, n, 1));
    const double across = std::sqrt(1.0 - (z * z));
    return {across * std::cos(turn), across * std::sin(turn), z};
}

terms::Context context_of(const hog::globe::Generator& generator) {
    return {.planet = &generator.planet(),
            .derived = &generator.derived(),
            .shape = &generator.shape()};
}

} // namespace

TEST_CASE("weights read between seed cells sum to 1, the nearest counting most",
          "[globe][cell_weights]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const terms::Context context = context_of(generator);
    const int seed = generator.shape().formed->seed_level();
    for (int level = seed; level <= seed + 4; ++level) {
        for (std::uint64_t n = 0; n < 100; ++n) {
            const Cell cell = hog::globe::cell_under(somewhere(n), level);
            hog::formation::Smooth::Weights weights;
            terms::weights_at(context, hog::globe::center(cell), weights);
            REQUIRE(!weights.points.empty());
            double sum = 0.0;
            for (const double weight : weights.weights) {
                sum += weight;
            }
            CHECK(std::abs(sum - 1.0) < 1e-9);
            const auto most = std::ranges::max_element(weights.weights);
            CHECK(weights.points.at(static_cast<std::size_t>(most - weights.weights.begin())) ==
                  weights.nearest);
        }
    }
}

TEST_CASE("ground read below the seed level stands as it's weighed there, over land and sea",
          "[globe][cell_weights]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const terms::Context context = context_of(generator);
    const hog::globe::Formed& formed = *generator.shape().formed;
    const int seed = generator.shape().formed->seed_level();
    int land = 0;
    int sea = 0;
    double worst = 0.0;
    double worst_rise = 0.0;
    for (int level = seed + 1; level <= seed + 5; ++level) {
        const double lod = terms::cell_lod(generator.planet(), level);
        // As many over land as over the sea, which covers most of it.
        int level_land = 0;
        int level_sea = 0;
        for (std::uint64_t n = 0; level_land < 60 && n < 5'000; ++n) {
            const Cell cell = hog::globe::cell_under(somewhere(n), level);
            const DVec3 middle = hog::globe::center(cell);
            const hog::globe::Formed::Here weighed = formed.at(middle, lod, true);
            const bool on_land = weighed.top.value > 0.0;
            if (!on_land && level_sea >= 60) {
                continue;
            }
            (on_land ? level_land : level_sea) += 1;
            (on_land ? land : sea) += 1;
            const terms::Ground read = terms::ground(context, cell, lod);
            worst = std::max(worst, std::abs(read.height - weighed.top.value));
            const DVec3 rise = terms::tangential(weighed.top.gradient, middle);
            worst_rise = std::max(worst_rise, glm::length(read.rise - rise));
        }
    }
    WARN("seed " << seed << " land " << land << " sea " << sea << " worst " << worst << " m, rise "
                 << worst_rise);
    CHECK(land == 300);
    CHECK(sea == 300);
    CHECK(worst < 0.25);
    CHECK(worst_rise < 0.02);
}

TEST_CASE("weights read between seed cells cost against weighing",
          "[.bench][globe][cell_weights]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::globe::Formed& formed = *generator.shape().formed;
    const hog::globe::WeightCells kept;
    terms::Context context = context_of(generator);
    context.weights = &kept;
    const int level = formed.seed_level() + 4;
    std::vector<DVec3> middles;
    for (std::uint64_t n = 0; n < 4'000; ++n) {
        middles.push_back(hog::globe::center(hog::globe::cell_under(somewhere(n % 40), level)) +
                          (somewhere(n) * 1e-5));
    }
    hog::formation::Smooth::Weights weights;
    double sink = 0.0;
    for (int round = 0; round < 3; ++round) {
        const auto started = std::chrono::steady_clock::now();
        for (const DVec3& middle : middles) {
            formed.weigh(glm::normalize(middle), weights);
            sink += weights.weights[0];
        }
        const auto weighed = std::chrono::steady_clock::now();
        for (const DVec3& middle : middles) {
            terms::weights_at(context, glm::normalize(middle), weights);
            sink += weights.weights[0];
        }
        const auto read = std::chrono::steady_clock::now();
        const auto each = [&](auto from, auto to) {
            return std::chrono::duration<double>(to - from).count() * 1e6 /
                   static_cast<double>(middles.size());
        };
        WARN("weighed " << each(started, weighed) << " us, read between " << each(weighed, read)
                        << " us");
    }
    CHECK(sink > 0.0);
}
