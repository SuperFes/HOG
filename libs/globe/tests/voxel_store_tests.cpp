#include <hog/globe/brick.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::Brick;
using hog::globe::BrickSite;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Voxel;
using hog::globe::VoxelStore;

namespace {

// The default planet's: 28,000 layers on columns of level 18.
constexpr hog::globe::Layers layers{
    .level = 18, .floor = 111'000.0, .thickness = 0.5, .count = 28'000};

Brick dug_out() {
    return Brick{Voxel{.temperature = 2'300}};
}

Brick half_filled() {
    std::array<Voxel, hog::globe::brick_voxels> voxels{};
    for (std::size_t index = 0; index < voxels.size(); ++index) {
        voxels[index] = index < 256 ? Voxel{.distance = -400, .solid = Material::Granite}
                                    : Voxel{.distance = 600};
    }
    return Brick::pack(voxels);
}

} // namespace

TEST_CASE("the store holds nothing until a brick is put", "[globe][store]") {
    const VoxelStore store{layers};
    CHECK(store.size() == 0);
    CHECK(store.bricks_across() == (1U << 18U) / 8);
    CHECK(store.bricks_up() == 3'500);
    CHECK_FALSE(store.at({.face = 1, .i = 9, .j = 9, .layer = 8'000}));
    CHECK(store.brick({.face = 5}) == nullptr);
}

TEST_CASE("a put brick answers for its voxels until let go", "[globe][store]") {
    VoxelStore store{layers};
    const BrickSite site{.face = 3, .i = 1'234, .j = 20'000, .layer = 1'000};
    store.put(site, half_filled());
    CHECK(store.size() == 1);
    const Site low = hog::globe::site_in(site, 3);
    const Site high = hog::globe::site_in(site, 300);
    CHECK(store.at(low) == Voxel{.distance = -400, .solid = Material::Granite});
    CHECK(store.at(high) == Voxel{.distance = 600});
    // Its neighbours are still as generated.
    CHECK_FALSE(store.at({.face = 3, .i = low.i, .j = low.j, .layer = low.layer + 8}));
    CHECK_FALSE(store.at({.face = 2, .i = low.i, .j = low.j, .layer = low.layer}));

    store.put(site, dug_out());
    CHECK(store.at(low) == Voxel{.temperature = 2'300});
    store.erase(site);
    CHECK(store.size() == 0);
    CHECK_FALSE(store.at(low));
}

TEST_CASE("the store tells which bricks changed, in order, once", "[globe][store]") {
    VoxelStore store{layers};
    const std::vector<BrickSite> sites{
        {.face = 0, .i = 0, .j = 0, .layer = 0},
        {.face = 0, .i = 0, .j = 0, .layer = 3'499},
        {.face = 2, .i = 32'767, .j = 5, .layer = 2},
        {.face = 5, .i = 32'767, .j = 32'767, .layer = 3'499},
    };
    for (const BrickSite& site : {sites[3], sites[1], sites[2], sites[0]}) {
        store.put(site, dug_out());
    }
    CHECK(store.sites() == sites);
    CHECK(store.take_changed() == sites);
    CHECK(store.take_changed().empty());

    store.erase(sites[2]);
    store.erase({.face = 1, .i = 7, .j = 7, .layer = 7}); // never held: no change
    store.put(sites[0], half_filled());
    CHECK(store.take_changed() == std::vector<BrickSite>{sites[0], sites[2]});
    CHECK(store.sites() == std::vector<BrickSite>{sites[0], sites[1], sites[3]});
}

TEST_CASE("a dug-out brick costs little to hold", "[globe][store]") {
    VoxelStore store{layers};
    for (std::uint32_t k = 0; k < 1'000; ++k) {
        store.put({.face = 1, .i = k, .j = 100, .layer = 900}, dug_out());
    }
    CHECK(store.bytes() / store.size() < 100);
    for (std::uint32_t k = 0; k < 1'000; ++k) {
        store.put({.face = 1, .i = k, .j = 100, .layer = 900}, half_filled());
    }
    CHECK(store.bytes() / store.size() < 1'000);
}
