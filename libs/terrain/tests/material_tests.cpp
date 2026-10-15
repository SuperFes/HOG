#include <hog/terrain/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using hog::DVec3;
using hog::terrain::Look;
using hog::terrain::material_image;
using hog::terrain::MaterialImage;

namespace {

constexpr int size = 64;

using Texel = std::array<std::uint8_t, 4>;

Texel texel_at(const std::vector<std::uint8_t>& level, int across, int x, int y) {
    const std::size_t first =
        4 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(across)) +
             static_cast<std::size_t>(x));
    return {level.at(first), level.at(first + 1), level.at(first + 2), level.at(first + 3)};
}

double decode(std::uint8_t byte) {
    const double encoded = byte / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

std::uint8_t encode(double linear) {
    const double encoded =
        linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
}

int difference(const Texel& a, const Texel& b) {
    int most = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        most = std::max(most, std::abs(int{a.at(channel)} - int{b.at(channel)}));
    }
    return most;
}

Look grassy() {
    return {.colour = {0.10, 0.20, 0.05},
            .accent = {0.25, 0.22, 0.08},
            .accent_share = 0.3,
            .grain = 0.3,
            .mottle = 0.2,
            .seed = 7};
}

} // namespace

TEST_CASE("a material has every level down to a single texel", "[terrain][material]") {
    const MaterialImage image = material_image(grassy(), size);
    CHECK(image.size == size);
    REQUIRE(image.levels.size() == 7);
    for (std::size_t level = 0; level < image.levels.size(); ++level) {
        const std::size_t across = static_cast<std::size_t>(size) >> level;
        CHECK(image.levels[level].size() == 4 * across * across);
    }
}

TEST_CASE("a material tiles without a seam", "[terrain][material]") {
    const MaterialImage image = material_image(grassy(), size);
    const std::vector<std::uint8_t>& level = image.levels.front();
    int inside = 0;
    int across_edge = 0;
    for (int along = 0; along < size; ++along) {
        for (int at = 0; at + 1 < size; ++at) {
            inside = std::max(
                {inside,
                 difference(texel_at(level, size, at, along), texel_at(level, size, at + 1, along)),
                 difference(texel_at(level, size, along, at),
                            texel_at(level, size, along, at + 1))});
        }
        across_edge = std::max(
            {across_edge,
             difference(texel_at(level, size, size - 1, along), texel_at(level, size, 0, along)),
             difference(texel_at(level, size, along, size - 1), texel_at(level, size, along, 0))});
    }
    CHECK(inside > 0);
    CHECK(across_edge <= inside);
}

TEST_CASE("a material averages to its look's colour, level by level", "[terrain][material]") {
    const MaterialImage image = material_image(grassy(), size);
    const Texel last = texel_at(image.levels.back(), 1, 0, 0);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(int{last.at(channel)} -
                       int{encode(grassy().colour[static_cast<glm::length_t>(channel)])}) <= 1);
    }
    CHECK(last[3] == 255);

    // Each texel is the four above it averaged in linear light, to within rounding.
    for (std::size_t level = 1; level < image.levels.size(); ++level) {
        const int across = size >> level;
        for (int y = 0; y < across; ++y) {
            for (int x = 0; x < across; ++x) {
                const Texel below = texel_at(image.levels[level], across, x, y);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    double sum = 0.0;
                    for (int corner = 0; corner < 4; ++corner) {
                        sum += decode(texel_at(image.levels[level - 1], across * 2,
                                               (2 * x) + (corner % 2), (2 * y) + (corner / 2))
                                          .at(channel));
                    }
                    INFO("level " << level << " at " << x << ", " << y);
                    CHECK(std::abs(int{below.at(channel)} - int{encode(sum / 4.0)}) <= 1);
                }
            }
        }
    }
}

TEST_CASE("a material's accent covers its share of the ground", "[terrain][material]") {
    const Look look{.colour = {0.5, 0.1, 0.1}, .accent = {0.1, 0.1, 0.5}, .accent_share = 0.3};
    const MaterialImage image = material_image(look, size);
    const std::vector<std::uint8_t>& level = image.levels.front();
    int bluer = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const Texel texel = texel_at(level, size, x, y);
            bluer += texel[2] > texel[0] ? 1 : 0;
        }
    }
    const double share = static_cast<double>(bluer) / (size * size);
    CHECK(share > 0.25);
    CHECK(share < 0.35);
}

TEST_CASE("a plain look makes a plain material", "[terrain][material]") {
    const Look plain{.colour = {0.3, 0.3, 0.2}};
    const MaterialImage image = material_image(plain, size);
    const std::vector<std::uint8_t>& level = image.levels.front();
    const Texel expected{encode(0.3), encode(0.3), encode(0.2), 255};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            CHECK(texel_at(level, size, x, y) == expected);
        }
    }
}

TEST_CASE("a material is the same each time it's made, and differs by seed",
          "[terrain][material]") {
    CHECK(material_image(grassy(), size).levels == material_image(grassy(), size).levels);
    Look reseeded = grassy();
    reseeded.seed = 8;
    CHECK(material_image(reseeded, size).levels.front() !=
          material_image(grassy(), size).levels.front());
}
