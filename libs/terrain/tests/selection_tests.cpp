#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <numbers>
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

// Flat ground at sea level, for horizons worked out by hand.
const hog::terrain::SelectionSettings flat_ground{.lowest = 0.0, .highest = 1.0};

// Looking along `forward` from `height` metres over `somewhere`, bent as walking there.
Viewpoint walking(double height, DVec3 forward) {
    const DVec3 eye = somewhere * (radius + height);
    Viewpoint view = looking(eye, forward);
    view.bend = hog::terrain::bend_from(view.eye, radius, 0.0);
    return view;
}

// The ground `metres` along it from under the eye, towards `towards`, level with the eye.
DVec3 along_ground(double metres, DVec3 towards) {
    const double turned = metres / radius;
    return (somewhere * std::cos(turned)) + (towards * std::sin(turned));
}

// Metres along the ground to the horizon of ground `ground` metres high, from `eye` metres
// high, over a sea-level sphere of radius `sphere`.
double horizon(double sphere, double eye, double ground) {
    return sphere * (std::acos(sphere / (sphere + eye)) + std::acos(sphere / (sphere + ground)));
}

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

TEST_CASE("ground known to lie low draws fewer patches from above", "[terrain][selection]") {
    const DVec3 eye = somewhere * (radius + 20'000.0);
    const auto view = looking(eye, (somewhere * radius) + (level_with(eye) * 30'000.0) - eye);
    const hog::terrain::KnownHeights lowland = [](CellId) {
        return hog::terrain::HeightRange{.lowest = 0.0, .highest = 50.0};
    };
    const auto assumed = select_patches(view, radius);
    const auto known = select_patches(view, radius, {}, lowland);
    REQUIRE(!known.empty());
    CHECK(known.size() < assumed.size());
    CHECK(finest(known) <= finest(assumed));
}

TEST_CASE("ground known to stand higher than assumed is drawn finer", "[terrain][selection]") {
    const DVec3 eye = somewhere * (radius + 12'000.0);
    const auto view = looking(eye, -somewhere);
    const hog::terrain::KnownHeights plateau = [](CellId) {
        return hog::terrain::HeightRange{.lowest = 11'000.0, .highest = 11'500.0};
    };
    CHECK(finest(select_patches(view, radius, {}, plateau)) > finest(select_patches(view, radius)));
}

TEST_CASE("ground known to lie low hides sooner past the horizon", "[terrain][selection]") {
    const DVec3 eye = somewhere * (radius + 100.0);
    const auto view = looking(eye, level_with(eye));
    const auto farthest = [&](const std::vector<SelectedPatch>& drawn) {
        double angle = 0.0;
        for (const SelectedPatch& patch : drawn) {
            angle = std::max(angle,
                             std::acos(glm::dot(hog::planet::cell_center(patch.cell), somewhere)));
        }
        return angle;
    };
    const hog::terrain::KnownHeights flat = [](CellId) {
        return hog::terrain::HeightRange{.lowest = 0.0, .highest = 1.0};
    };
    CHECK(farthest(select_patches(view, radius, {}, flat)) <
          farthest(select_patches(view, radius)));
}

TEST_CASE("bent, ground out to the faked horizon is picked and beyond it isn't",
          "[terrain][selection]") {
    const DVec3 ahead = level_with(somewhere);
    const Viewpoint bent = walking(3.0, ahead);
    const Viewpoint unbent = looking(bent.eye.metres(), ahead);
    const double faked = horizon(bent.bend.faked, 3.0, 1.0);
    REQUIRE(faked > 5.0 * horizon(radius, 3.0, 1.0));

    const auto drawn = select_patches(bent, radius, flat_ground);
    const auto at = [](double metres, DVec3 towards) {
        return hog::planet::cell_at(along_ground(metres, towards));
    };
    CHECK(drawn_over(drawn, at(0.9 * faked, ahead)));
    CHECK(!drawn_over(select_patches(unbent, radius, flat_ground), at(0.9 * faked, ahead)));
    CHECK(!drawn_over(drawn, at(2.0 * faked, ahead)));
    CHECK(!drawn_over(drawn, at(2'000.0, -ahead)));
}

TEST_CASE("bent, all the ground drawn in view is picked", "[terrain][selection]") {
    const DVec3 ahead = level_with(somewhere);
    const DVec3 aside = glm::cross(somewhere, ahead);
    std::mt19937_64 random{11};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    for (const double pitch : {0.0, 0.2, 0.45}) {
        const Viewpoint view =
            walking(3.0, (ahead * std::cos(pitch)) - (somewhere * std::sin(pitch)));
        const auto drawn = select_patches(view, radius, flat_ground);
        std::vector<SelectedPatch> by_cell = drawn;
        std::ranges::sort(by_cell, {}, &SelectedPatch::cell);

        const DVec3 right = glm::normalize(glm::cross(view.forward, view.up));
        const DVec3 up = glm::cross(right, view.forward);
        const double tall = std::tan(view.vertical_fov / 2.0);
        int seen = 0;
        for (int sample = 0; sample < 4'000; ++sample) {
            // Short of the sea-level horizon, so in sight, and often near the view's edges.
            const double metres = horizon(view.bend.faked, 3.0, 0.0) * std::sqrt(unit(random));
            const double bearing = 2.0 * std::numbers::pi * unit(random);
            const DVec3 towards = (ahead * std::cos(bearing)) + (aside * std::sin(bearing));
            const DVec3 ground = along_ground(metres, towards) * (radius + unit(random));
            const DVec3 drawn_at = hog::terrain::bent(view.bend, ground - view.eye.metres());
            const double depth = glm::dot(drawn_at, view.forward);
            if (depth <= 0.0 || std::abs(glm::dot(drawn_at, up)) > depth * tall ||
                std::abs(glm::dot(drawn_at, right)) > depth * tall * view.aspect) {
                continue;
            }
            ++seen;
            CHECK(sorted_over(by_cell, hog::planet::cell_at(ground)));
        }
        CHECK(seen > 200);
    }
}

TEST_CASE("bent, the ground under a walker is still drawn finest", "[terrain][selection]") {
    const DVec3 ahead = level_with(somewhere);
    const Viewpoint bent = walking(2.0, ahead);
    const auto under = [&](const Viewpoint& view) {
        return drawn_over(select_patches(view, radius), hog::planet::cell_at(somewhere))
            .value_or(SelectedPatch{});
    };
    const SelectedPatch found = under(bent);
    CHECK(found.distance == 0.0);
    CHECK(found.cell == under(looking(bent.eye.metres(), ahead)).cell);
}

TEST_CASE("blended out, the bend changes nothing picked", "[terrain][selection]") {
    const DVec3 ahead = level_with(somewhere);
    const Viewpoint high = walking(20'000.0, ahead - somewhere);
    REQUIRE(high.bend.faked == radius);
    const auto cells = [](const std::vector<SelectedPatch>& drawn) {
        std::vector<CellId> picked;
        for (const SelectedPatch& patch : drawn) {
            picked.push_back(patch.cell);
        }
        return picked;
    };
    CHECK(cells(select_patches(high, radius)) ==
          cells(select_patches(looking(high.eye.metres(), ahead - somewhere), radius)));
}
