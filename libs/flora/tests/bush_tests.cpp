#include <hog/flora/bushes.hpp>
#include <hog/flora/fates.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

using hog::flora::bush_grown;
using hog::flora::BushKind;
using hog::flora::Fate;
using hog::planet::Biome;

namespace {

// A wooded stretch of face 0, as the trees' own tests take.
constexpr std::uint32_t first_i = 8192;
constexpr std::uint32_t first_j = 226304;
constexpr std::uint32_t side = 128;

struct Grown {
    std::size_t bushes = 0;
    std::size_t floofy = 0;
    std::size_t spiky = 0;
    std::size_t twiggy = 0;
    std::size_t mats = 0;
};

Grown grown_as(Biome biome, const Fate& fate = Fate{}) {
    Grown grown;
    for (std::uint32_t i = first_i; i < first_i + side; ++i) {
        for (std::uint32_t j = first_j; j < first_j + side; ++j) {
            const auto bush = bush_grown(i, j, 0, biome, fate);
            if (!bush) {
                continue;
            }
            ++grown.bushes;
            switch (bush->kind) {
            case BushKind::Floofy:
                ++grown.floofy;
                break;
            case BushKind::Spiky:
                ++grown.spiky;
                break;
            case BushKind::Twiggy:
                ++grown.twiggy;
                break;
            case BushKind::Mat:
                ++grown.mats;
                break;
            }
        }
    }
    return grown;
}

constexpr double cells = static_cast<double>(side * side);

} // namespace

TEST_CASE("bushes stand by the same dice on every machine", "[flora][bushes]") {
    // As the trees': if this number moves, the same world grows different bushes on different
    // machines or releases.
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    const auto fold = [&hash](std::uint32_t bits) {
        hash ^= bits;
        hash *= 0x100'0000'01b3ULL;
    };
    std::size_t grown = 0;
    for (std::uint32_t i = first_i; i < first_i + 64; ++i) {
        for (std::uint32_t j = first_j; j < first_j + 64; ++j) {
            const auto bush = bush_grown(i, j, 0, Biome::TemperateForest);
            const hog::Vec2 stray = hog::flora::bush_stray(i, j, 0);
            fold(std::bit_cast<std::uint32_t>(stray.x));
            fold(std::bit_cast<std::uint32_t>(stray.y));
            if (!bush) {
                continue;
            }
            ++grown;
            fold(bush->hash);
            fold(std::bit_cast<std::uint32_t>(bush->height));
            fold(std::bit_cast<std::uint32_t>(bush->width));
            fold(std::bit_cast<std::uint32_t>(bush->spot.x));
            fold(std::bit_cast<std::uint32_t>(bush->spot.y));
            fold(static_cast<std::uint32_t>(bush->kind));
        }
    }
    CHECK(grown > 200);
    CHECK(hash == 0xc4c1'c68b'64ef'a6f3ULL);
}

TEST_CASE("bushes grow as their biome grows them, in its own mix of kinds", "[flora][bushes]") {
    // Open ground, no woods to crowd or shade them: the biome's share, its kinds by their own.
    const Grown tundra = grown_as(Biome::Tundra);
    CHECK(static_cast<double>(tundra.bushes) / cells > 0.27);
    CHECK(static_cast<double>(tundra.bushes) / cells < 0.33);
    CHECK(tundra.floofy == 0);
    CHECK(tundra.spiky == 0);
    CHECK(static_cast<double>(tundra.twiggy) / static_cast<double>(tundra.bushes) > 0.25);
    CHECK(static_cast<double>(tundra.twiggy) / static_cast<double>(tundra.bushes) < 0.35);

    const Grown desert = grown_as(Biome::Desert);
    CHECK(static_cast<double>(desert.bushes) / cells > 0.06);
    CHECK(static_cast<double>(desert.bushes) / cells < 0.10);
    CHECK(desert.spiky > desert.twiggy);

    for (const Biome none : {Biome::Ocean, Biome::Beach, Biome::Ice, Biome::Lake}) {
        CHECK(grown_as(none).bushes == 0);
    }
}

TEST_CASE("a bush's kind sets its build: mats low and wide, rosettes as wide as tall",
          "[flora][bushes]") {
    bool mat = false;
    bool spiky = false;
    for (std::uint32_t j = first_j; j < first_j + side; ++j) {
        const auto bush = bush_grown(first_i, j, 0, Biome::Shrubland);
        if (!bush) {
            continue;
        }
        const float tallest = hog::flora::bushes_of(Biome::Shrubland).height * 1.3F;
        CHECK(bush->height <= tallest);
        CHECK(bush->spot.x >= 0.0F);
        CHECK(bush->spot.x < 1.0F);
        if (bush->kind == BushKind::Mat) {
            mat = true;
            CHECK(bush->height <= tallest * 0.4F);
            CHECK(bush->width > bush->height * 2.0F);
        } else if (bush->kind == BushKind::Spiky) {
            spiky = true;
            CHECK(bush->width == bush->height);
        }
    }
    CHECK(mat);
    CHECK(spiky);
}

TEST_CASE("bushes crowd in where the dead trees have fallen, and not on black ground",
          "[flora][bushes]") {
    const std::size_t woods = grown_as(Biome::TemperateForest).bushes;
    // The trees gone, the dead still standing: the canopy's shade lifted, no more.
    const std::size_t standing =
        grown_as(Biome::TemperateForest, {.living = 0.0F, .established = 0.0F, .dead = 1.0F})
            .bushes;
    const std::size_t fallen =
        grown_as(Biome::TemperateForest, {.living = 0.0F, .established = 0.0F, .dead = 0.0F})
            .bushes;
    INFO("woods " << woods << ", dead standing " << standing << ", fallen " << fallen);
    CHECK(standing >= woods);
    CHECK(fallen > standing + (standing / 2));
    CHECK(grown_as(Biome::TemperateForest, {.living = 0.0F, .established = 0.0F, .scar = 1.0F})
              .bushes == 0);
    // Where no trees grow, their fates touch nothing but the black.
    CHECK(grown_as(Biome::Tundra, {.living = 0.0F, .established = 0.0F}).bushes ==
          grown_as(Biome::Tundra).bushes);
}

TEST_CASE("a bush strays evenly about its spot, within the biomes' blending reach",
          "[flora][bushes]") {
    double sum_x = 0.0;
    double sum_y = 0.0;
    std::size_t far = 0;
    for (std::uint32_t i = first_i; i < first_i + side; ++i) {
        const hog::Vec2 stray = hog::flora::bush_stray(i, first_j, 0);
        CHECK(stray.x >= -1.0F);
        CHECK(stray.x < 1.0F);
        CHECK(stray.y >= -1.0F);
        CHECK(stray.y < 1.0F);
        sum_x += static_cast<double>(stray.x);
        sum_y += static_cast<double>(stray.y);
        far += stray.x > 0.5F ? 1U : 0U;
    }
    CHECK(std::abs(sum_x / side) < 0.15);
    CHECK(std::abs(sum_y / side) < 0.15);
    CHECK(far > side / 8);
}
