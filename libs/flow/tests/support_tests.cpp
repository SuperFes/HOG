#include <hog/flow/support.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::flow::Support;
using hog::globe::Material;

namespace {

// A box of 40 by 40 columns and 20 layers a half metre thick, with a floor of `ground` two
// layers deep.
Support floored(Material ground = Material::Granite) {
    Support box{40, 40, 20, 0.5, 9.81};
    for (int j = 0; j < 40; ++j) {
        for (int i = 0; i < 40; ++i) {
            for (int layer = 0; layer < 2; ++layer) {
                box.at(i, j, layer).solid = ground;
            }
        }
    }
    return box;
}

void fill(Support& box, int i0, int i1, int j0, int j1, int l0, int l1, Material material) {
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            for (int layer = l0; layer <= l1; ++layer) {
                box.at(i, j, layer).solid = material;
            }
        }
    }
}

} // namespace

TEST_CASE("matter standing on the ground holds itself up", "[flow][support]") {
    Support box = floored();
    fill(box, 10, 20, 10, 20, 2, 12, Material::Granite);
    CHECK(box.failing().empty());
}

TEST_CASE("matter touching nothing falls", "[flow][support]") {
    Support box = floored();
    fill(box, 10, 14, 10, 14, 8, 10, Material::Granite);
    const auto failed = box.failing();
    REQUIRE_FALSE(failed.empty());
    CHECK(failed.size() == 25);
}

TEST_CASE("a short overhang stands and a long one of the same rock breaks off",
          "[flow][support]") {
    const double reach = floored().reach(Material::Sandstone, 1.0, 255);
    const int short_by = static_cast<int>(0.5 * reach / 0.5);
    const int long_by = static_cast<int>(1.5 * reach / 0.5);
    REQUIRE(long_by < 28);
    // A pillar with a slab out from its top, a metre deep.
    const auto slab = [](int length) {
        Support box = floored();
        fill(box, 5, 7, 18, 22, 2, 9, Material::Sandstone);
        fill(box, 8, 7 + length, 18, 22, 8, 9, Material::Sandstone);
        return box;
    };
    CHECK(slab(short_by).failing().empty());
    CHECK_FALSE(slab(long_by).failing().empty());
}

TEST_CASE("weak ground reaches less than strong, and worn ground less than sound",
          "[flow][support]") {
    Support box = floored();
    const double strong = box.reach(Material::Granite, 1.0, 255);
    CHECK(box.reach(Material::Sand, 1.0, 255) < strong);
    CHECK(box.reach(Material::Granite, 1.0, 64) < strong);
    CHECK(box.reach(Material::Granite, 4.0, 255) > strong);
}

TEST_CASE("a bridge held at both ends spans what one held at one end cannot", "[flow][support]") {
    // A slab 1 m deep of weak ground, longer than it can reach from one side but within twice.
    Support probe = floored();
    const double reach = probe.reach(Material::Sandstone, 1.0, 255);
    const int cells = static_cast<int>(1.5 * reach / 0.5);
    REQUIRE(cells < 36);
    Support bridge = floored();
    fill(bridge, 2, 4, 18, 22, 2, 9, Material::Sandstone);
    fill(bridge, 5 + cells + 0, 7 + cells, 18, 22, 2, 9, Material::Sandstone);
    fill(bridge, 2, 7 + cells, 18, 22, 8, 9, Material::Sandstone);
    Support cantilever = floored();
    fill(cantilever, 2, 4, 18, 22, 2, 9, Material::Sandstone);
    fill(cantilever, 2, 7 + cells, 18, 22, 8, 9, Material::Sandstone);
    CHECK(bridge.failing().empty());
    CHECK_FALSE(cantilever.failing().empty());
}
