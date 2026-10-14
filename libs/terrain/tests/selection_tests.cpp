#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <vector>

using hog::DVec3;
using hog::planet::CellId;
using hog::terrain::select_patches;
using hog::terrain::SelectedPatch;
using hog::terrain::Viewpoint;

namespace {

constexpr double radius = 115'000.0;

Viewpoint looking(DVec3 eye, DVec3 forward) {
    forward = glm::normalize(forward);
    const DVec3 up = std::abs(forward.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    return {.eye = hog::WorldPosition{eye}, .forward = forward, .up = up};
}

// A direction along the ground at `eye`.
DVec3 level_with(DVec3 eye) {
    return glm::normalize(glm::cross(eye, DVec3{0.0, 0.0, 1.0}));
}

std::optional<SelectedPatch> drawn_over(const std::vector<SelectedPatch>& drawn, CellId cell) {
    const auto found = std::ranges::find_if(
        drawn, [&](const SelectedPatch& patch) { return patch.cell.contains(cell); });
    return found == drawn.end() ? std::nullopt : std::optional{*found};
}

// As drawn_over, for patches sorted by cell: they don't overlap, so the one over `cell` sorts
// next to it.
std::optional<SelectedPatch> sorted_over(const std::vector<SelectedPatch>& by_cell, CellId cell) {
    const auto after = std::ranges::lower_bound(by_cell, cell, {}, &SelectedPatch::cell);
    if (after != by_cell.end() && after->cell.contains(cell)) {
        return *after;
    }
    if (after != by_cell.begin() && std::prev(after)->cell.contains(cell)) {
        return *std::prev(after);
    }
    return std::nullopt;
}

int finest(const std::vector<SelectedPatch>& drawn) {
    int level = 0;
    for (const SelectedPatch& patch : drawn) {
        level = std::max(level, patch.cell.level());
    }
    return level;
}

const DVec3 somewhere = hog::planet::direction_at(0.3, 0.5);

} // namespace

TEST_CASE("from far off, the near side is drawn coarsely", "[terrain][selection]") {
    const DVec3 out{1.0, 0.0, 0.0};
    const auto drawn = select_patches(looking(out * radius * 20.0, -out), radius);
    REQUIRE(!drawn.empty());
    CHECK(finest(drawn) <= 1);
    CHECK(drawn_over(drawn, hog::planet::cell_at(out)));
    CHECK(!drawn_over(drawn, hog::planet::cell_at(-out)));
}

TEST_CASE("nearer views draw finer patches", "[terrain][selection]") {
    int previous = -1;
    // Above the highest the ground might reach; below it, the ground could be anywhere near.
    for (const double height : {1e6, 3e5, 1e5, 3e4, 1e4}) {
        const DVec3 eye = somewhere * (radius + height);
        const auto drawn = select_patches(looking(eye, -somewhere), radius);
        const int level = finest(drawn);
        CHECK(level > previous);
        previous = level;
    }
}

TEST_CASE("the ground under a walker is drawn finest", "[terrain][selection]") {
    const DVec3 eye = somewhere * (radius + 2.0);
    const hog::terrain::SelectionSettings settings;
    const auto drawn = select_patches(looking(eye, level_with(eye)), radius, settings);
    const auto found = drawn_over(drawn, hog::planet::cell_at(somewhere));
    REQUIRE(found.has_value());
    const SelectedPatch under = found.value_or(SelectedPatch{});
    CHECK(under.distance == 0.0);
    CHECK(drawn.front().distance == 0.0);
    // Fine enough to stop splitting, and no finer.
    const auto corners = hog::planet::cell_vertices(under.cell);
    const double spacing =
        glm::distance(corners[0], corners[1]) * radius / hog::terrain::patch_cells;
    CHECK(spacing <= settings.finest_spacing);
    CHECK(spacing > settings.finest_spacing / 4.0);
    // Rings of patches coarsening outwards, not a carpet of fine ones.
    std::map<int, int> at_level;
    for (const SelectedPatch& patch : drawn) {
        ++at_level[patch.cell.level()];
    }
    for (const auto& [level, count] : at_level) {
        CHECK(count < 512);
    }
}

TEST_CASE("neighbouring patches differ by a level at most, and say which", "[terrain][selection]") {
    std::mt19937_64 random{7};
    std::uniform_real_distribution<double> unit{-1.0, 1.0};
    const auto any_direction = [&] {
        DVec3 direction{};
        do {
            direction = {unit(random), unit(random), unit(random)};
        } while (glm::length(direction) < 0.1);
        return glm::normalize(direction);
    };
    int seams = 0;
    for (int view = 0; view < 24; ++view) {
        const double height = std::pow(10.0, 1.0 + (2.5 * (unit(random) + 1.0)));
        const DVec3 up = any_direction();
        const DVec3 eye = up * (radius + height);
        const DVec3 target = glm::normalize(up + (0.3 * any_direction())) * radius;
        const auto drawn = select_patches(looking(eye, target - eye), radius);
        REQUIRE(!drawn.empty());
        auto by_cell = drawn;
        std::ranges::sort(by_cell, {}, &SelectedPatch::cell);
        for (const SelectedPatch& patch : drawn) {
            const auto neighbours = hog::planet::edge_neighbors(patch.cell);
            for (std::size_t edge = 0; edge < neighbours.size(); ++edge) {
                const bool coarser = ((patch.coarser_edges >> edge) & 1U) != 0;
                // Every neighbouring pair is seen from the finer side, as the coarser covers it.
                if (const auto over = sorted_over(by_cell, neighbours.at(edge))) {
                    CHECK(over->cell.level() >= patch.cell.level() - 1);
                    CHECK(coarser == (over->cell.level() < patch.cell.level()));
                    seams += coarser ? 1 : 0;
                }
            }
        }
    }
    CHECK(seams > 0);
}

TEST_CASE("drawn patches don't overlap, and come nearest first", "[terrain][selection]") {
    const DVec3 eye = somewhere * (radius + 300.0);
    auto drawn = select_patches(looking(eye, level_with(eye)), radius);
    CHECK(std::ranges::is_sorted(drawn, {}, &SelectedPatch::distance));
    std::ranges::sort(drawn, {}, &SelectedPatch::cell);
    for (std::size_t at = 1; at < drawn.size(); ++at) {
        CHECK(drawn[at - 1].cell.range_max() < drawn[at].cell.range_min());
    }
}

TEST_CASE("looking away from the planet draws nothing", "[terrain][selection]") {
    CHECK(select_patches(looking(somewhere * radius * 2.0, somewhere), radius).empty());
}

TEST_CASE("ground below the horizon is left out", "[terrain][selection]") {
    // Looking straight down, the far side lies in the view, behind the planet.
    const DVec3 eye = somewhere * (radius + 100.0);
    const auto drawn = select_patches(looking(eye, -somewhere), radius);
    REQUIRE(!drawn.empty());
    CHECK(!drawn_over(drawn, hog::planet::cell_at(-somewhere)));
    for (const SelectedPatch& patch : drawn) {
        CHECK(glm::dot(hog::planet::cell_center(patch.cell), somewhere) > 0.0);
    }
}

TEST_CASE("a ground range out of order is refused", "[terrain][selection]") {
    const hog::testing::AssertionGuard guard;
    const hog::terrain::SelectionSettings upside_down{.lowest = 10.0, .highest = -10.0};
    CHECK_THROWS_AS(
        select_patches(looking(somewhere * radius * 2.0, -somewhere), radius, upside_down),
        hog::testing::AssertionFailed);
}
