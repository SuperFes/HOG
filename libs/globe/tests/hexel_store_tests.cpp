#include <hog/globe/accretion.hpp>
#include <hog/globe/birth.hpp>
#include <hog/globe/hexel_store.hpp>
#include <hog/globe/settling.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::HexCell;
using hog::globe::Hexel;
using hog::globe::HexelStore;
using hog::globe::Material;

namespace {

// Metre hexels on the game's planet: 170 billion columns, 75 thousand shells.
constexpr int fine = 17;
const hog::globe::Shells shells{.inner = 0.0, .outer = 143'750.0, .count = 74'870};
constexpr int ground = 72'000;

const Hexel rock{.material = Material::Granite, .temperature = 300};
const Hexel sky{.material = Material::Air, .temperature = 250};

HexelStore layered() {
    HexelStore store{fine, shells};
    store.fill(0, ground, rock);
    store.fill(ground, shells.count, sky);
    return store;
}

HexCell somewhere(std::uint64_t k) {
    return hog::globe::cell_at_index(fine,
                                     (k * 0x9E37'79B9'7F4AULL) % hog::globe::cell_count(fine));
}

} // namespace

TEST_CASE("a store of one hexel throughout is a node a diamond, at any level", "[globe][store]") {
    const HexelStore store{24, {.inner = 0.0, .outer = 1e5, .count = 1'000'000}, rock};
    CHECK(store.nodes() == 12);
    CHECK(store.at(hog::globe::cell_at_index(24, 123'456'789), 999'999) == rock);
    CHECK(store.at(HexCell::pole(24, false), 0) == rock);
}

TEST_CASE("a layered planet of metre hexels holds in a few kilobytes", "[globe][store]") {
    const HexelStore store = layered();
    CHECK(store.bytes() < 16'384);
    for (std::uint64_t k = 0; k < 1'000; ++k) {
        CHECK(store.at(somewhere(k), 0) == rock);
        CHECK(store.at(somewhere(k), ground - 1) == rock);
        CHECK(store.at(somewhere(k), ground) == sky);
        CHECK(store.at(somewhere(k), shells.count - 1) == sky);
    }
    CHECK(store.at(HexCell::pole(fine, true), ground) == sky);
}

TEST_CASE("digging a hexel out costs a path and a brick, and filling it back gives them up",
          "[globe][store]") {
    HexelStore store = layered();
    const std::size_t before = store.bytes();
    const HexCell cell = somewhere(7);
    const auto beside = hog::globe::neighbors(cell);
    store.set(cell, ground - 1, sky);
    CHECK(store.at(cell, ground - 1) == sky);
    CHECK(store.at(cell, ground - 2) == rock);
    for (const HexCell next : beside.all()) {
        CHECK(store.at(next, ground - 1) == rock);
    }
    CHECK(store.bytes() < before + 4'096);
    store.set(cell, ground - 1, rock);
    store.compact();
    CHECK(store.bytes() == before);
    CHECK(store.at(cell, ground - 1) == rock);
}

TEST_CASE("painting over a block and compacting lets the old one go", "[globe][store]") {
    HexelStore store{6, {.inner = 0.0, .outer = 1e5, .count = 64}};
    for (std::uint64_t index = 0; index < hog::globe::cell_count(6); index += 3) {
        store.set(hog::globe::cell_at_index(6, index), 5, rock);
    }
    CHECK(store.nodes() > 12);
    store.fill(0, 64, sky);
    store.compact();
    CHECK(store.nodes() == 12);
    CHECK(store.at(hog::globe::cell_at_index(6, 3), 5) == sky);
}

TEST_CASE("a born planet packs and unpacks to the same hexels", "[globe][store]") {
    const hog::globe::Globe born = hog::globe::birth({.seed = 3, .level = 4});
    const HexelStore store = hog::globe::pack(born);
    hog::globe::Globe back{born.level(), born.shells()};
    hog::globe::unpack(store, back);
    for (std::uint64_t index = 0; index < hog::globe::cell_count(born.level()); ++index) {
        const auto want = born.column_at(index);
        const auto got = back.column_at(index);
        for (std::size_t shell = 0; shell < want.size(); ++shell) {
            REQUIRE(got[shell] == want[shell]);
        }
    }
}

TEST_CASE("sand pours into a shaft dug in a window, and the store keeps it", "[globe][store]") {
    HexelStore store{fine, shells};
    const Hexel sand{.material = Material::Sand, .temperature = 280};
    store.fill(0, ground - 30, rock);
    store.fill(ground - 30, ground, sand);
    store.fill(ground, shells.count, sky);
    const HexCell middle = somewhere(11);
    constexpr int from = ground - 60;
    hog::globe::Globe near =
        hog::globe::window(store, hog::globe::disc(middle, 30), from, ground + 20);
    CHECK(near.size() == 1 + (3 * 30 * 31));
    CHECK(near.first_shell() == from);
    CHECK(near.at(middle, ground - 1 - from) == sand);
    CHECK(near.at(middle, ground - from) == sky);

    // Twenty shells of sand carried out of the middle column.
    for (int shell = ground - 20; shell < ground; ++shell) {
        near.at(middle, shell - from) = sky;
    }
    const auto before = near.masses();
    const std::vector<HexCell> dug{middle};
    CHECK(hog::globe::settle(near, dug, {.surface = 9.81, .radius = 115'000.0}) > 0);
    CHECK(near.masses() == before);
    const int top = hog::globe::top_of(near.column(middle)) + from;
    CHECK(top >= ground - 4);
    CHECK(top < ground);

    hog::globe::put_back(near, store);
    CHECK(hog::globe::top_of(near.column(middle)) + from == top);
    CHECK(store.at(middle, top - 1) == sand);
    CHECK(store.at(middle, top) == sky);
    CHECK(store.at(somewhere(12), ground - 1) == sand);
    CHECK(store.bytes() < 1'000'000);
}
