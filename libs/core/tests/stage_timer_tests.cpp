#include <hog/core/stage_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {

// A clock that moves on only when told.
struct FakeClock {
    hog::MonotonicClock::time_point at;

    [[nodiscard]] hog::StageTimer::Now now() {
        return [this] { return at; };
    }
};

} // namespace

TEST_CASE("stages add up the time of every run", "[core][stage_timer]") {
    FakeClock clock;
    hog::StageTimer timer{clock.now()};
    timer.time("plates", [&] { clock.at += 2s; });
    timer.time("sea", [&] { clock.at += 500ms; });
    timer.time("plates", [&] { clock.at += 1s; });

    const auto stages = timer.stages();
    REQUIRE(stages.size() == 2);
    CHECK(stages[0].name == "plates");
    CHECK(stages[0].took == 3s);
    CHECK(stages[0].runs == 2);
    CHECK(stages[1].name == "sea");
    CHECK(stages[1].took == 500ms);
    CHECK(stages[1].runs == 1);
}

TEST_CASE("a timed stage hands back what it returns", "[core][stage_timer]") {
    hog::StageTimer timer;
    CHECK(timer.time("answer", [] { return 42; }) == 42);
    CHECK(timer.stages().size() == 1);
}

TEST_CASE("a stage that throws is still timed", "[core][stage_timer]") {
    FakeClock clock;
    hog::StageTimer timer{clock.now()};
    CHECK_THROWS(timer.time("failing", [&] {
        clock.at += 1s;
        throw std::runtime_error{"failed"};
    }));
    REQUIRE(timer.stages().size() == 1);
    CHECK(timer.stages()[0].took == 1s);
}

TEST_CASE("the report lines up each stage and the total", "[core][stage_timer]") {
    FakeClock clock;
    hog::StageTimer timer{clock.now()};
    timer.time("plates", [&] { clock.at += 1500ms; });
    timer.time("sea", [&] { clock.at += 250ms; });
    timer.time("sea", [&] { clock.at += 250ms; });
    CHECK(timer.report() == "plates     1.500 s\n"
                            "sea        0.500 s  (2 runs)\n"
                            "total      2.000 s\n");
}
