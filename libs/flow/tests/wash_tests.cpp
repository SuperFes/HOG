#include <hog/flow/wash.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using Catch::Approx;
using hog::flow::Wash;
using hog::globe::Material;

namespace {

// A line of columns, each draining into the one before it, the first under still water: a
// slope `steep` rise a column, each column loose to `depth` metres of `material` over its rock.
std::vector<Wash::Node> slope(std::size_t count, double steep, Material material, double depth,
                              double discharge) {
    std::vector<Wash::Node> nodes(count);
    for (std::size_t n = 0; n < count; ++n) {
        nodes[n].ground = static_cast<double>(n) * steep;
        nodes[n].loose = {{.material = material, .depth = depth}};
        nodes[n].discharge = discharge * static_cast<double>(count - n);
        nodes[n].drains = static_cast<int>(n) - 1;
    }
    nodes[0].still = true;
    nodes[0].drains = Wash::none;
    return nodes;
}

double loose_in(const Wash& wash) {
    double total = 0.0;
    for (std::size_t n = 0; n < wash.size(); ++n) {
        for (const Wash::Piece& piece : wash[n].loose) {
            total += piece.depth;
        }
    }
    return total;
}

} // namespace

TEST_CASE("running water carries topsoil off a slope into the still water at its foot",
          "[flow][wash]") {
    Wash wash{slope(10, 0.3, Material::Soil, 1.0, 2'000.0), {.cell = 2.0}};
    const double before = loose_in(wash);
    wash.run(1'000.0, 8);
    CHECK(wash.taken(5) > 0.0);
    CHECK(wash[5].ground < 5.0 * 0.3);
    // All of it ends in the pool, as silt.
    CHECK(wash[0].loose.back().material == Material::Sediment);
    CHECK(wash[0].loose.back().laid);
    CHECK(loose_in(wash) == Approx(before));
}

TEST_CASE("clay holds against water that strips topsoil, and stones against both", "[flow][wash]") {
    const auto taken_of = [](Material material) {
        Wash wash{slope(10, 0.3, material, 1.0, 20.0), {.cell = 2.0}};
        wash.run(200.0, 8);
        return wash.taken(5);
    };
    CHECK(taken_of(Material::Clay) < taken_of(Material::Soil));
    CHECK(taken_of(Material::Gravel) < taken_of(Material::Clay));
}

TEST_CASE("water takes the loose ground down to the rock and no further", "[flow][wash]") {
    Wash wash{slope(6, 1.0, Material::Sand, 0.2, 1.0e5), {.cell = 2.0}};
    wash.run(1'000.0, 8);
    for (std::size_t n = 1; n < 6; ++n) {
        CAPTURE(n);
        CHECK(wash[n].ground >= static_cast<double>(n) - 0.2 - 1e-9);
    }
    CHECK(wash.taken(5) == Approx(0.2));
}

TEST_CASE("where the slope eases the water drops its stones first and carries the fines on",
          "[flow][wash]") {
    // Steep above column 6, near level below it, down to the pool.
    std::vector<Wash::Node> nodes = slope(12, 0.02, Material::Gravel, 0.0, 2'000.0);
    for (std::size_t n = 6; n < 12; ++n) {
        nodes[n].ground = (6.0 * 0.02) + (static_cast<double>(n - 5) * 1.0);
        nodes[n].loose = {{.material = Material::Gravel, .depth = 0.5},
                          {.material = Material::Soil, .depth = 0.5}};
    }
    Wash wash{nodes, {.cell = 2.0}};
    wash.run(2'000.0, 8);
    bool stones = false;
    for (std::size_t n = 1; n <= 5; ++n) {
        for (const Wash::Piece& piece : wash[n].loose) {
            stones = stones || (piece.laid && piece.material == Material::Gravel);
        }
    }
    CHECK(stones);
    bool silt = false;
    for (const Wash::Piece& piece : wash[0].loose) {
        silt = silt || piece.material == Material::Sediment;
    }
    CHECK(silt);
}

TEST_CASE("what runs out of the set is carried away", "[flow][wash]") {
    std::vector<Wash::Node> nodes = slope(6, 0.5, Material::Soil, 1.0, 2'000.0);
    nodes[0].still = false;
    Wash wash{nodes, {.cell = 2.0}};
    const double before = loose_in(wash);
    wash.run(1'000.0, 8);
    CHECK(loose_in(wash) < before);
}
