#include <hog/terrain/water.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using hog::DVec3;
using hog::terrain::Carried;
using hog::terrain::open_sea;
using hog::terrain::Optics;
using hog::terrain::optics;

namespace {

double brightest(DVec3 colour) {
    return std::max({colour.x, colour.y, colour.z});
}

// How far down a bed still shows a fiftieth of what it would, in the colour that gets through
// best: looking straight down, down to it and back up.
double seen_down_to(const Optics& water) {
    const DVec3 lost = water.dimming + water.hiding;
    return 3.9 / std::min({lost.x, lost.y, lost.z});
}

} // namespace

TEST_CASE("clear water loses the red first and turns blue", "[terrain][water]") {
    for (const Carried& carried : {Carried{}, open_sea}) {
        const Optics water = optics(carried);
        CHECK(water.dimming.x > water.dimming.y);
        CHECK(water.dimming.y > water.dimming.z);
        CHECK(water.colour.z > water.colour.y);
        CHECK(water.colour.y > water.colour.x);
    }
    // Tens of metres down in the open sea.
    CHECK(seen_down_to(optics(open_sea)) > 20.0);
    CHECK(seen_down_to(optics(open_sea)) < 60.0);
}

TEST_CASE("the more water carries, the less is seen through it", "[terrain][water]") {
    const Optics clear = optics(open_sea);
    for (const Carried& more : {Carried{.organics = 1.0}, Carried{.sediment = 5.0},
                                Carried{.plankton = 5.0}, Carried{.sediment = 5.0, .flour = 1.0}}) {
        const Optics water = optics(more);
        CHECK(water.dimming.x > clear.dimming.x);
        CHECK(water.hiding.y > clear.hiding.y);
        CHECK(water.hiding.z > clear.hiding.z);
    }
}

TEST_CASE("water stained by peat and forest litter turns dark tea-brown", "[terrain][water]") {
    const Optics tea = optics({.organics = 5.0, .sediment = 1.0, .plankton = 2.0});
    CHECK(tea.dimming.z > tea.dimming.y);
    CHECK(tea.dimming.y > tea.dimming.x);
    CHECK(tea.colour.x > tea.colour.z);
    CHECK(brightest(tea.colour) < brightest(optics(open_sea).colour));
    // Down a metre or two at most.
    CHECK(seen_down_to(tea) < 3.0);
}

TEST_CASE("muddy water is brown, bright and hides the bed within a hand", "[terrain][water]") {
    const Optics mud = optics({.organics = 0.5, .sediment = 200.0, .plankton = 1.0});
    CHECK(mud.colour.x > mud.colour.y);
    CHECK(mud.colour.y > mud.colour.z);
    CHECK(brightest(mud.colour) > brightest(optics(open_sea).colour));
    CHECK(seen_down_to(mud) < 0.2);
}

TEST_CASE("rock flour turns glacier water milky turquoise", "[terrain][water]") {
    const Optics glacial =
        optics({.organics = 0.1, .sediment = 5.0, .plankton = 0.3, .flour = 1.0});
    const Optics silty = optics({.organics = 0.1, .sediment = 5.0, .plankton = 0.3});
    CHECK(glacial.colour.y > 1.5 * glacial.colour.x);
    CHECK(glacial.colour.z > 1.5 * glacial.colour.x);
    CHECK(brightest(glacial.colour) > brightest(silty.colour));
    CHECK(glacial.colour.z / glacial.colour.x > silty.colour.z / silty.colour.x);
    CHECK(seen_down_to(glacial) > 0.5);
    CHECK(seen_down_to(glacial) < 5.0);
}

TEST_CASE("a bloom of plankton turns water green", "[terrain][water]") {
    const Optics bloom = optics({.organics = 0.5, .sediment = 1.0, .plankton = 30.0});
    CHECK(bloom.colour.y > bloom.colour.z);
    CHECK(bloom.colour.y > bloom.colour.x);
}

TEST_CASE("nothing carried counts as less than none, nor flour as more than all the sediment",
          "[terrain][water]") {
    const auto same = [](const Optics& a, const Optics& b) {
        return a.dimming == b.dimming && a.hiding == b.hiding && a.colour == b.colour;
    };
    CHECK(same(optics({.plankton = -1.0}), optics({})));
    CHECK(same(optics({.sediment = 5.0, .flour = 2.0}), optics({.sediment = 5.0, .flour = 1.0})));
}
