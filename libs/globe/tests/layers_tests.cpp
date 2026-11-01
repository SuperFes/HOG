#include <hog/globe/layers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

using hog::globe::Layers;

TEST_CASE("layers stand evenly from the floor to the ceiling", "[globe][layers]") {
    const Layers layers = hog::globe::layers_about(115'000.0, 3'000.0, 2'000.0, 0.5);
    CHECK(layers.count == 10'000);
    CHECK(layers.floor == 112'000.0);
    CHECK(std::abs(layers.ceiling() - 117'000.0) < 1e-9);
    CHECK(layers.layer_at(111'999.0) == -1);
    CHECK(layers.layer_at(112'000.0) == 0);
    CHECK(layers.layer_at(115'000.25) == 6'000);
    CHECK(layers.layer_at(117'000.0) == layers.count);
    for (int layer = 0; layer < layers.count; layer += 97) {
        CHECK(layers.layer_at((layers.bottom(layer) + layers.top(layer)) / 2.0) == layer);
    }
}

TEST_CASE("a layer's voxels fill its spherical shell exactly", "[globe][layers]") {
    const Layers layers = hog::globe::layers_about(10'000.0, 50.0, 50.0, 10.0);
    constexpr int level = 4;
    for (const int layer : {0, 5, layers.count - 1}) {
        double total = 0.0;
        for (std::uint64_t index = 0; index < hog::globe::cells_at(level); ++index) {
            total += layers.volume(hog::globe::cell_at_index(level, index), layer);
        }
        const auto cube = [](double r) { return r * r * r; };
        const double shell =
            4.0 / 3.0 * std::numbers::pi * (cube(layers.top(layer)) - cube(layers.bottom(layer)));
        CHECK(std::abs(total - shell) < 1e-9 * shell);
    }
}

TEST_CASE("columns stand on the level whose cells are about a layer wide", "[globe][layers]") {
    CHECK(hog::globe::layers_about(115'000.0, 3'000.0, 2'000.0, 0.5).level == 18);
    for (const double thickness : {0.25, 0.5, 1.0, 3.0}) {
        const Layers layers = hog::globe::layers_about(115'000.0, 3'000.0, 2'000.0, thickness);
        const double width =
            std::sqrt(4.0 * std::numbers::pi / 6.0) * 115'000.0 / std::ldexp(1.0, layers.level);
        CHECK(width <= layers.thickness * std::numbers::sqrt2);
        CHECK(width > layers.thickness / std::numbers::sqrt2);
    }
}
