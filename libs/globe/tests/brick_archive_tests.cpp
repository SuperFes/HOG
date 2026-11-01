#include <hog/globe/brick.hpp>
#include <hog/globe/brick_archive.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

using hog::globe::Brick;
using hog::globe::BrickSite;
using hog::globe::Material;
using hog::globe::MemoryBrickArchive;
using hog::globe::Voxel;
using hog::globe::VoxelStore;

namespace {

constexpr hog::globe::Layers layers{
    .level = 18, .floor = 111'000.0, .thickness = 0.5, .count = 28'000};

// A brick of water filling it to some height, the rest air.
Brick pond(std::uint16_t top) {
    std::array<Voxel, hog::globe::brick_voxels> voxels{};
    for (int index = 0; index < hog::globe::brick_voxels; ++index) {
        const auto layer = static_cast<std::uint16_t>(index / 64);
        Voxel& voxel = voxels[static_cast<std::size_t>(index)];
        voxel.liquid = Material::Water;
        voxel.liquid_amount = layer < top ? Voxel::full : static_cast<std::uint16_t>(index % 7);
        voxel.temperature = 2'300;
    }
    return Brick::pack(voxels);
}

// Every voxel the store holds, by brick.
std::map<BrickSite, std::array<Voxel, hog::globe::brick_voxels>> held(const VoxelStore& store) {
    std::map<BrickSite, std::array<Voxel, hog::globe::brick_voxels>> voxels;
    for (const BrickSite& site : store.sites()) {
        store.brick(site)->unpack(voxels[site]);
    }
    return voxels;
}

// Liquid held, by what it is.
std::map<Material, std::uint64_t> liquids(const VoxelStore& store) {
    std::map<Material, std::uint64_t> amounts;
    for (const auto& [site, voxels] : held(store)) {
        for (const Voxel& voxel : voxels) {
            amounts[voxel.liquid] += voxel.liquid_amount;
        }
    }
    return amounts;
}

} // namespace

TEST_CASE("a saved store loads back voxel for voxel", "[globe][archive]") {
    VoxelStore store{layers};
    store.put({.face = 0, .i = 10, .j = 20, .layer = 1'000}, pond(3));
    store.put({.face = 5, .i = 99, .j = 1, .layer = 1'001}, pond(8));
    store.put({.face = 2, .i = 7, .j = 7, .layer = 0}, Brick{Voxel{.solid = Material::Basalt}});
    MemoryBrickArchive archive;
    CHECK(hog::globe::save(store, archive) == 3);
    CHECK(store.changed().empty());
    CHECK(hog::globe::save(store, archive) == 0);

    const auto loaded = hog::globe::load(layers, archive);
    REQUIRE(loaded);
    CHECK(held(*loaded) == held(store));
    CHECK(loaded->changed().empty());
    // Water is counted to the last part in 32,768 across a save.
    CHECK(liquids(*loaded) == liquids(store));
}

TEST_CASE("a save writes only what changed since the last", "[globe][archive]") {
    VoxelStore store{layers};
    const BrickSite kept{.face = 1, .i = 4, .j = 4, .layer = 900};
    const BrickSite changed{.face = 1, .i = 5, .j = 4, .layer = 900};
    const BrickSite dropped{.face = 1, .i = 6, .j = 4, .layer = 900};
    for (const BrickSite& site : {kept, changed, dropped}) {
        store.put(site, pond(2));
    }
    MemoryBrickArchive archive;
    REQUIRE(hog::globe::save(store, archive));
    const auto before = archive.bricks().at(hog::globe::brick_key(kept));

    store.put(changed, pond(6));
    store.erase(dropped);
    CHECK(hog::globe::save(store, archive) == 2);
    CHECK(archive.bricks().size() == 2);
    CHECK(archive.bricks().at(hog::globe::brick_key(kept)) == before);
    const auto loaded = hog::globe::load(layers, archive);
    REQUIRE(loaded);
    CHECK(held(*loaded) == held(store));
}

TEST_CASE("dug and built ground loads back as it was left", "[globe][archive]") {
    const hog::globe::Generator generator{
        hog::globe::preset("moon").value_or(hog::globe::Planet{})};
    VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
    const hog::DVec3 direction = glm::normalize(hog::DVec3{0.3, -0.5, 0.8});
    const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0);
    hog::globe::dig(world, direction * ground, 3.0);
    hog::globe::build(world, direction * (ground + 4.0), 1.5, Material::Basalt);
    REQUIRE(store.size() > 0);

    MemoryBrickArchive archive;
    REQUIRE(hog::globe::save(store, archive));
    auto loaded = hog::globe::load(generator.layers(), archive);
    REQUIRE(loaded);
    CHECK(held(*loaded) == held(store));
    const hog::globe::World again{generator, *loaded};
    const hog::globe::Cell cell = hog::globe::cell_under(direction, generator.layers().level);
    const hog::globe::Site middle{
        .face = cell.face,
        .i = cell.i,
        .j = cell.j,
        .layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground))};
    CHECK(again.at(middle) == world.at(middle));
    CHECK(again.at(middle).distance > 0);
}

TEST_CASE("bricks saved for other layers or damaged fail to load", "[globe][archive]") {
    VoxelStore store{layers};
    store.put({.face = 3, .i = 1, .j = 2, .layer = 3}, pond(4));
    MemoryBrickArchive archive;
    REQUIRE(hog::globe::save(store, archive));

    hog::globe::Layers thinner = layers;
    thinner.thickness = 0.25;
    CHECK_FALSE(hog::globe::load(thinner, archive));

    SECTION("a brick cut short") {
        archive.bricks().begin()->second.pop_back();
        CHECK_FALSE(hog::globe::load(layers, archive));
    }
    SECTION("a brick with bytes over") {
        archive.bricks().begin()->second.push_back(std::byte{0});
        CHECK_FALSE(hog::globe::load(layers, archive));
    }
    SECTION("a brick off the planet") {
        auto bytes = archive.bricks().begin()->second;
        archive.bricks().emplace(hog::globe::brick_key({.face = 3, .i = 1, .j = 2, .layer = 4'000}),
                                 std::move(bytes));
        CHECK_FALSE(hog::globe::load(layers, archive));
    }
    SECTION("a newer format") {
        MemoryBrickArchive newer;
        const std::array header{std::byte{hog::globe::brick_format + 1}, std::byte{0}};
        REQUIRE(newer.write(header, {}, {}));
        const auto loaded = hog::globe::load(layers, newer);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == hog::ErrorCode::Unsupported);
    }
}

TEST_CASE("an archive never written loads an empty store", "[globe][archive]") {
    const MemoryBrickArchive archive;
    const auto loaded = hog::globe::load(layers, archive);
    REQUIRE(loaded);
    CHECK(loaded->size() == 0);
}

TEST_CASE("a brick coded to send is taken into another store, or let go without bytes",
          "[globe][archive]") {
    VoxelStore ours{layers};
    VoxelStore theirs{layers};
    const BrickSite site{.face = 2, .i = 5, .j = 6, .layer = 7};
    ours.put(site, pond(3));
    const hog::globe::SavedBrick coded = hog::globe::coded_brick(ours, site);
    CHECK(coded.key == hog::globe::brick_key(site));
    CHECK_FALSE(coded.bytes.empty());
    REQUIRE(hog::globe::take_brick(theirs, coded));
    CHECK(held(theirs) == held(ours));
    // Let go on one side, it's let go on the other.
    ours.erase(site);
    const hog::globe::SavedBrick gone = hog::globe::coded_brick(ours, site);
    CHECK(gone.bytes.empty());
    REQUIRE(hog::globe::take_brick(theirs, gone));
    CHECK(theirs.size() == 0);
    // A key of no brick, or bytes that aren't one, are refused.
    CHECK_FALSE(hog::globe::take_brick(theirs, {.key = ~std::uint64_t{0}, .bytes = coded.bytes}));
    CHECK_FALSE(hog::globe::take_brick(theirs, {.key = coded.key, .bytes = {std::byte{7}}}));
}
