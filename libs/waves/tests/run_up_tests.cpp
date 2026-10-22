#include <hog/waves/run_up.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using Catch::Matchers::WithinRel;

TEST_CASE("waves run up a beach as Stockdon found", "[waves][run_up]") {
    // Two-metre waves of ten seconds on a beach falling one in twenty.
    const hog::waves::RunUp run = hog::waves::run_up(2.0, 10.0, 0.05);
    const double scale = std::sqrt(2.0 * hog::waves::gravity * 100.0 / (2.0 * std::numbers::pi));
    CHECK_THAT(run.setup, WithinRel(0.35 * 0.05 * scale, 1e-12));
    const double swash = std::sqrt(scale * scale * ((0.563 * 0.05 * 0.05) + 0.004));
    CHECK_THAT(std::hypot(run.incident, run.infragravity), WithinRel(swash, 1e-12));
    CHECK_THAT(hog::waves::run_up_reach(run), WithinRel(1.1 * (run.setup + (swash / 2.0)), 1e-12));
    // On the gentlest beaches, near enough their own fit, mostly the groups' slow surges.
    const hog::waves::RunUp flat = hog::waves::run_up(2.0, 10.0, 0.0);
    CHECK_THAT(hog::waves::run_up_reach(flat), WithinRel(0.043 * scale, 0.15));
    CHECK(flat.infragravity > 5.0 * flat.incident);
    CHECK(flat.setup == hog::waves::run_up(2.0, 10.0, hog::waves::gentlest_beach).setup);
}

TEST_CASE("bigger, longer waves on steeper beaches run up higher", "[waves][run_up]") {
    const double reach = hog::waves::run_up_reach(hog::waves::run_up(1.5, 8.0, 0.04));
    CHECK(hog::waves::run_up_reach(hog::waves::run_up(3.0, 8.0, 0.04)) > reach);
    CHECK(hog::waves::run_up_reach(hog::waves::run_up(1.5, 14.0, 0.04)) > reach);
    CHECK(hog::waves::run_up_reach(hog::waves::run_up(1.5, 8.0, 0.1)) > reach);
    CHECK(hog::waves::run_up_reach(hog::waves::run_up(0.0, 8.0, 0.1)) == 0.0);
}
