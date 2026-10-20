#include <hog/client/woodlands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::client::Woodlands;
using hog::planet::CellId;
using hog::protocol::CellWoodland;

namespace {

hog::protocol::Woodlands told(std::uint8_t level) {
    return {.level = level,
            .cells = std::vector<CellWoodland>(hog::protocol::weather_cells(level))};
}

} // namespace

TEST_CASE("the woods' fates change only where the server says they do, and beside it",
          "[client][woodlands]") {
    Woodlands woodlands;
    CHECK(woodlands.fates()->untouched());
    // Told untouched woods, nothing changes.
    CHECK(woodlands.receive(told(2)).empty());
    CHECK_FALSE(woodlands.fates()->untouched());

    // One cell burned, in the middle of face 1: it and the eight about it change.
    hog::protocol::Woodlands burned = told(2);
    const std::uint32_t middle = (16 * 1) + (4 * 1) + 1;
    burned.cells[middle] = {
        .living = 0.1F, .established = 0.1F, .dead = 0.8F, .charred = 1.0F, .scar = 0.6F};
    const std::vector<CellId> changed = woodlands.receive(burned);
    CHECK(changed.size() == 9);
    CHECK(std::ranges::all_of(changed, [](CellId cell) {
        const hog::planet::FaceIJ at = cell.to_face_ij();
        return at.face == 1 && at.level == 2 && at.i <= 2 && at.j <= 2;
    }));
    CHECK(woodlands.fates()->cell(middle).living == 0.1F);
    CHECK(woodlands.fates()->cell(middle).scar == 0.6F);
    CHECK(woodlands.fates()->at(1, 1.5 / 4.0, 1.5 / 4.0).dead == 0.8F);
    // Told the same again, nothing changes; a corner cell's change reaches only its face's cells.
    CHECK(woodlands.receive(burned).empty());
    burned.cells[0].living = 0.5F;
    const std::vector<CellId> corner = woodlands.receive(burned);
    CHECK(corner.size() == 4);
    // Told at another level, everything changes.
    CHECK(woodlands.receive(told(1)).size() == hog::protocol::weather_cells(1));
}
