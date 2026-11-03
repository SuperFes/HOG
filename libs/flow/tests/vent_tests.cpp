#include <hog/flow/vent.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::flow::rate_at;
using hog::flow::Stage;
using hog::flow::stage_at;
using hog::flow::Vent;

TEST_CASE("a vent erupts, seeps, then sleeps in a cycle", "[flow][vent]") {
    const Vent vent{.site = {},
                    .active_rate = 0.5,
                    .erupting_rate = 20.0,
                    .period = 1'000.0,
                    .eruption = 100.0,
                    .active = 300.0};
    CHECK(stage_at(vent, 0.0) == Stage::Erupting);
    CHECK(stage_at(vent, 99.0) == Stage::Erupting);
    CHECK(stage_at(vent, 100.0) == Stage::Active);
    CHECK(stage_at(vent, 399.0) == Stage::Active);
    CHECK(stage_at(vent, 400.0) == Stage::Dormant);
    CHECK(stage_at(vent, 999.0) == Stage::Dormant);
    CHECK(stage_at(vent, 1'050.0) == Stage::Erupting);
    CHECK(rate_at(vent, 50.0) == 20.0);
    CHECK(rate_at(vent, 200.0) == 0.5);
    CHECK(rate_at(vent, 600.0) == 0.0);
}

TEST_CASE("a vent's phase shifts its cycle, and a plain vent is always active", "[flow][vent]") {
    Vent vent{.site = {}, .period = 1'000.0, .eruption = 100.0, .active = 0.0, .phase = 500.0};
    CHECK(stage_at(vent, 0.0) == Stage::Dormant);
    CHECK(stage_at(vent, 500.0) == Stage::Erupting);
    CHECK(stage_at(vent, -500.0) == Stage::Erupting);
    const Vent lake{.site = {}, .active_rate = 0.2};
    CHECK(stage_at(lake, 12'345.0) == Stage::Active);
    CHECK(rate_at(lake, 12'345.0) == 0.2);
}
