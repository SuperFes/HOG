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

// A brick the ground's surface runs through: amounts differ across it, three rocks, some water.
Voxels surface() {
    Voxels voxels{};
    for (int index = 0; index < brick_voxels; ++index) {
        const hog::globe::Site site = hog::globe::site_in({}, index);
        Voxel& voxel = voxels[static_cast<std::size_t>(index)];
        const double distance = (static_cast<double>(site.layer) * 0.5) -
                                (static_cast<double>(site.i) * 0.037) -
                                (static_cast<double>(site.j) * 0.011) - 1.5;
        voxel.solid_amount = hog::globe::amount_at(distance, 2.0);
        if (distance < 0.0) {
            voxel.solid = site.i < 4 ? Material::Soil : Material::Sand;
            if (site.layer < 2) {
                voxel.solid = Material::Granite;
            }
        } else if (site.layer < 5) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount = Voxel::full;
            voxel.rise = static_cast<std::uint8_t>(site.i);
            voxel.peak = static_cast<std::uint8_t>(site.j * 9);
        }
        voxel.temperature = static_cast<std::uint16_t>(2'300 + site.layer);
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
    voxels.fill({.solid = Material::Basalt, .temperature = 2'400, .solid_amount = Voxel::full});
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
    // The kind, then the solid's.
    no_material[1] = std::byte{200};
    hog::ByteReader material{no_material};
    CHECK_FALSE(Brick::read(material));
}

TEST_CASE("a brick written when voxels kept a distance and a normal still reads",
          "[globe][brick]") {
    hog::ByteWriter out;
    out.write(std::uint8_t{0});       // a whole brick of the old kind
    out.write(std::uint16_t{0x8001}); // its distance
    out.write(std::uint16_t{0x8080}); // its normal
    out.write(static_cast<std::uint8_t>(Material::Basalt));
    out.write(std::uint8_t{Voxel::sound});
    out.write(static_cast<std::uint8_t>(Material::Air));
    out.write(std::uint16_t{0});
    out.write(static_cast<std::uint8_t>(Material::Air));
    out.write(std::uint16_t{0});
    out.write(std::uint16_t{2'400});
    out.write(std::uint16_t{Voxel::full});
    const std::vector<std::byte> bytes = std::move(out).take();
    hog::ByteReader in{bytes};
    const auto read = Brick::read(in);
    REQUIRE(read);
    CHECK(read->is_uniform());
    CHECK(read->at(0) ==
          Voxel{.solid = Material::Basalt, .temperature = 2'400, .solid_amount = Voxel::full});
}

TEST_CASE("a brick written before water rose and fell with the year reads with none",
          "[globe][brick]") {
    hog::ByteWriter out;
    out.write(std::uint8_t{2}); // a whole brick of that kind
    out.write(static_cast<std::uint8_t>(Material::Air));
    out.write(std::uint8_t{Voxel::sound});
    out.write(static_cast<std::uint8_t>(Material::Air));
    out.write(std::uint16_t{0});
    out.write(static_cast<std::uint8_t>(Material::Water));
    out.write(std::uint16_t{Voxel::full});
    out.write(std::uint16_t{2'300});
    out.write(std::uint16_t{0});
    const std::vector<std::byte> bytes = std::move(out).take();
    hog::ByteReader in{bytes};
    const auto read = Brick::read(in);
    REQUIRE(read);
    CHECK(in.remaining() == 0);
    CHECK(read->at(0) ==
          Voxel{.liquid = Material::Water, .liquid_amount = Voxel::full, .temperature = 2'300});
}
