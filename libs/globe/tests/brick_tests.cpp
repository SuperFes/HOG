#include <hog/core/bytes.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::globe::Brick;
using hog::globe::brick_voxels;
using hog::globe::Material;
using hog::globe::Voxel;

namespace {

using Voxels = std::array<Voxel, brick_voxels>;

// A brick the ground's surface runs through: every distance differs, three rocks, some water.
Voxels surface() {
    Voxels voxels{};
    for (int index = 0; index < brick_voxels; ++index) {
        const hog::globe::Site site = hog::globe::site_in({}, index);
        Voxel& voxel = voxels[static_cast<std::size_t>(index)];
        voxel.distance = static_cast<std::int16_t>((static_cast<int>(site.layer) * 500) -
                                                   (static_cast<int>(site.i) * 37) -
                                                   (static_cast<int>(site.j) * 11) - 1'500);
        if (voxel.distance < 0) {
            voxel.solid = site.i < 4 ? Material::Soil : Material::Sand;
            if (site.layer < 2) {
                voxel.solid = Material::Granite;
            }
        } else if (site.layer < 5) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount = Voxel::full;
        }
        voxel.temperature = static_cast<std::uint16_t>(2'300 + site.layer);
        voxel.normal =
            hog::globe::packed_normal(glm::normalize(hog::DVec3{0.1 * site.i, 0.05 * site.j, 1.0}));
    }
    return voxels;
}

Voxels unpacked(const Brick& brick) {
    Voxels voxels{};
    brick.unpack(voxels);
    return voxels;
}

std::vector<std::byte> written(const Brick& brick) {
    hog::ByteWriter out;
    brick.write(out);
    return std::move(out).take();
}

} // namespace

TEST_CASE("a brick finds each voxel where its site says", "[globe][brick]") {
    const hog::globe::BrickSite brick{.face = 4, .i = 3, .j = 9, .layer = 700};
    for (int index = 0; index < brick_voxels; ++index) {
        const hog::globe::Site site = hog::globe::site_in(brick, index);
        CHECK(hog::globe::brick_of(site) == brick);
        CHECK(hog::globe::index_in_brick(site) == index);
    }
}

TEST_CASE("a brick of one voxel throughout is held as that voxel", "[globe][brick]") {
    Voxels voxels{};
    voxels.fill({.distance = -Voxel::far, .solid = Material::Basalt, .temperature = 2'400});
    const Brick brick = Brick::pack(voxels);
    CHECK(brick.is_uniform());
    CHECK(brick.bytes() == sizeof(Brick));
    CHECK(unpacked(brick) == voxels);
    CHECK(written(brick).size() < 20);
}

TEST_CASE("a brick packs each quantity apart and gives back every voxel", "[globe][brick]") {
    const Voxels voxels = surface();
    const Brick brick = Brick::pack(voxels);
    CHECK_FALSE(brick.is_uniform());
    CHECK(unpacked(brick) == voxels);
    for (int index = 0; index < brick_voxels; index += 13) {
        CHECK(brick.at(index) == voxels[static_cast<std::size_t>(index)]);
    }
    // Distances in full, a byte a voxel for the rest.
    CHECK(brick.bytes() < sizeof(Voxels) / 2);

    Brick copy;
    copy = brick;
    CHECK(copy == brick);
    Voxels changed = voxels;
    changed[100].condition = 3;
    CHECK(Brick::pack(changed) != brick);
}

TEST_CASE("a brick reads back as it was written", "[globe][brick]") {
    Voxels uniform{};
    uniform.fill({.liquid = Material::Water, .liquid_amount = Voxel::full, .temperature = 2'200});
    Voxels two = uniform;
    two[511].liquid_amount = 7;
    for (const Voxels& voxels : {uniform, two, surface()}) {
        const Brick brick = Brick::pack(voxels);
        const std::vector<std::byte> bytes = written(brick);
        hog::ByteReader in{bytes};
        const auto read = Brick::read(in);
        REQUIRE(read);
        CHECK(in.remaining() == 0);
        CHECK(*read == brick);
        CHECK(unpacked(*read) == voxels);
    }
    CHECK(written(Brick::pack(two)).size() < 200);
}

TEST_CASE("a damaged brick fails to read rather than read wrong", "[globe][brick]") {
    const std::vector<std::byte> bytes = written(Brick::pack(surface()));
    for (std::size_t cut = 0; cut < bytes.size(); cut += 97) {
        hog::ByteReader in{std::span{bytes}.first(cut)};
        CHECK_FALSE(Brick::read(in));
    }
    std::vector<std::byte> unknown = bytes;
    unknown[0] = std::byte{9};
    hog::ByteReader kind{unknown};
    CHECK_FALSE(Brick::read(kind));

    Voxels voxels{};
    voxels.fill({.solid = Material::Granite});
    std::vector<std::byte> no_material = written(Brick::pack(voxels));
    // The kind, the distance's two bytes and the normal's two, then the solid's.
    no_material[5] = std::byte{200};
    hog::ByteReader material{no_material};
    CHECK_FALSE(Brick::read(material));
}
