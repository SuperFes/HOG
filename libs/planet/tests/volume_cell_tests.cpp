#include <hog/planet/projection.hpp>
#include <hog/planet/volume_cell.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

using hog::planet::CellId;
using hog::planet::max_shell_level;
using hog::planet::volume_span;
using hog::planet::VolumeCell;

namespace {

constexpr double radius = 1'000'000.0;

[[nodiscard]] VolumeCell random_cell(std::mt19937_64& random) {
    std::uniform_int_distribution<int> face(0, hog::planet::face_count - 1);
    std::uniform_int_distribution<int> level(0, 20);
    std::uniform_int_distribution<int> shell_level(0, max_shell_level);
    const int at = level(random);
    const int shells = shell_level(random);
    const std::uint32_t side = 1U << at;
    std::uniform_int_distribution<std::uint32_t> across(0, side - 1);
    std::uniform_int_distribution<std::uint32_t> outward(0, (1U << shells) - 1);
    const CellId column = CellId::from_face_ij(face(random), across(random), across(random), at);
    return VolumeCell::from(column, shells, outward(random));
}

} // namespace

static_assert(VolumeCell::from(CellId::from_face(2), 3, 5).shell() == 5);
static_assert(VolumeCell::from(CellId::from_face(2), 3, 5).shell_level() == 3);
static_assert(!VolumeCell{}.is_valid());

TEST_CASE("shell and shell level round-trip exhaustively at low levels", "[planet][volume]") {
    const CellId column = CellId::from_face_ij(4, 2, 1, 2);
    for (int level = 0; level <= 8; ++level) {
        for (std::uint32_t shell = 0; shell < (1U << level); ++shell) {
            const VolumeCell cell = VolumeCell::from(column, level, shell);
            REQUIRE(cell.is_valid());
            REQUIRE(cell.column() == column);
            REQUIRE(cell.shell_level() == level);
            REQUIRE(cell.shell() == shell);
        }
    }
    const VolumeCell finest =
        VolumeCell::from(column, max_shell_level, (1U << max_shell_level) - 1);
    CHECK(finest.shell() == (1U << max_shell_level) - 1);
}

TEST_CASE("cells split across and outward on their own", "[planet][volume]") {
    std::mt19937_64 random{7};
    for (int n = 0; n < 2000; ++n) {
        const VolumeCell cell = random_cell(random);
        if (cell.column().level() < hog::planet::max_level) {
            for (int index = 0; index < 4; ++index) {
                const VolumeCell child = cell.across_child(index);
                REQUIRE(child.across_parent() == cell);
                REQUIRE(child.shell_level() == cell.shell_level());
                REQUIRE(child.shell() == cell.shell());
                REQUIRE(cell.contains(child));
                REQUIRE_FALSE(child.contains(cell));
            }
        }
        if (cell.shell_level() < max_shell_level) {
            for (int index = 0; index < 2; ++index) {
                const VolumeCell child = cell.shell_child(index);
                REQUIRE(child.shell_parent() == cell);
                REQUIRE(child.column() == cell.column());
                REQUIRE(child.shell() == (cell.shell() * 2) + static_cast<std::uint32_t>(index));
                REQUIRE(cell.contains(child));
            }
            REQUIRE(cell.shell_child(0) < cell.shell_child(1));
        }
    }
}

TEST_CASE("cells don't contain their neighbours", "[planet][volume]") {
    const CellId column = CellId::from_face_ij(1, 3, 3, 3);
    const VolumeCell cell = VolumeCell::from(column, 4, 6);
    CHECK_FALSE(cell.contains(VolumeCell::from(column, 4, 5)));
    CHECK_FALSE(cell.contains(VolumeCell::from(column, 4, 7)));
    CHECK_FALSE(cell.contains(VolumeCell::from(CellId::from_face_ij(1, 2, 3, 3), 4, 6)));
    CHECK(cell.contains(VolumeCell::from(column.child(2), 9, (6U << 5) + 31)));
}

TEST_CASE("a column's cells sort from the bottom up", "[planet][volume]") {
    const CellId column = CellId::from_face(3);
    for (std::uint32_t shell = 0; shell + 1 < 64; ++shell) {
        REQUIRE(VolumeCell::from(column, 6, shell) < VolumeCell::from(column, 6, shell + 1));
    }
}

TEST_CASE("shells tile the volume exactly", "[planet][volume]") {
    const auto whole = hog::planet::shell_bounds(VolumeCell::from(CellId::from_face(0)), radius);
    CHECK(whole.inner == radius - (volume_span / 2.0));
    CHECK(whole.outer == radius + (volume_span / 2.0));

    std::mt19937_64 random{11};
    for (int n = 0; n < 2000; ++n) {
        const VolumeCell cell = random_cell(random);
        if (cell.shell_level() == max_shell_level) {
            continue;
        }
        const auto bounds = hog::planet::shell_bounds(cell, radius);
        const auto lower = hog::planet::shell_bounds(cell.shell_child(0), radius);
        const auto upper = hog::planet::shell_bounds(cell.shell_child(1), radius);
        REQUIRE(lower.inner == bounds.inner);
        REQUIRE(lower.outer == upper.inner);
        REQUIRE(upper.outer == bounds.outer);
    }
}

TEST_CASE("the cell at a cell's centre is that cell", "[planet][volume]") {
    std::mt19937_64 random{13};
    for (int n = 0; n < 2000; ++n) {
        const VolumeCell cell = random_cell(random);
        const auto center = hog::planet::volume_center(cell, radius);
        REQUIRE(hog::planet::volume_cell_at(center, radius, cell.column().level(),
                                            cell.shell_level()) == cell);
    }
}

TEST_CASE("nothing outside the volume has a cell", "[planet][volume]") {
    const hog::DVec3 up{0.0, 0.0, 1.0};
    const hog::WorldPosition below{up * (radius - (volume_span / 2.0) - 1.0)};
    const hog::WorldPosition above{up * (radius + (volume_span / 2.0))};
    const hog::WorldPosition bottom{up * (radius - (volume_span / 2.0))};
    CHECK_FALSE(hog::planet::volume_cell_at(below, radius, 5, 5).is_valid());
    CHECK_FALSE(hog::planet::volume_cell_at(above, radius, 5, 5).is_valid());
    const VolumeCell lowest = hog::planet::volume_cell_at(bottom, radius, 5, 5);
    REQUIRE(lowest.is_valid());
    CHECK(lowest.shell() == 0);
    CHECK(lowest.column() == hog::planet::cell_at(up, 5));
}
