#include <hog/server/pacer.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;
using hog::server::Pacer;

namespace {

constexpr Pacer::TimePoint t0{1'000s};

} // namespace

TEST_CASE("ticks fall due at their deadlines", "[server][pacer]") {
    Pacer pacer{t0, 20, 5};
    CHECK(pacer.due(t0 - 1ms) == 0);
    CHECK(pacer.due(t0) == 1);
    CHECK(pacer.due(t0) == 0);
    CHECK(pacer.next_deadline() == t0 + 50ms);
    CHECK(pacer.due(t0 + 49ms) == 0);
    CHECK(pacer.due(t0 + 50ms) == 1);
    CHECK(pacer.due(t0 + 200ms) == 3);
    CHECK(pacer.next_deadline() == t0 + 250ms);
    CHECK(pacer.slipped() == 0);
}

TEST_CASE("deadlines are exact at rates that do not divide a second", "[server][pacer]") {
    Pacer pacer{t0, 30, 1};
    for (int i = 0; i < 300; ++i) {
        const Pacer::TimePoint deadline = pacer.next_deadline();
        REQUIRE(pacer.due(deadline - 1ns) == 0);
        REQUIRE(pacer.due(deadline) == 1);
    }
    CHECK(pacer.next_deadline() == t0 + 10s);
    CHECK(pacer.slipped() == 0);
}

TEST_CASE("a stalled pacer drops what it cannot catch up", "[server][pacer]") {
    Pacer pacer{t0, 20, 5};
    CHECK(pacer.due(t0 + 10s) == 5);
    CHECK(pacer.slipped() == 196);
    CHECK(pacer.next_deadline() == t0 + 10s + 50ms);
    CHECK(pacer.due(t0 + 10s + 50ms) == 1);
}

TEST_CASE("a rate change starts from the next deadline", "[server][pacer]") {
    Pacer pacer{t0, 20, 5};
    CHECK(pacer.due(t0) == 1);
    pacer.set_rate(10);
    CHECK(pacer.next_deadline() == t0 + 50ms);
    CHECK(pacer.due(t0 + 50ms) == 1);
    CHECK(pacer.next_deadline() == t0 + 150ms);
    CHECK(pacer.due(t0 + 1s + 50ms) == 5);
    CHECK(pacer.slipped() == 5);
}

TEST_CASE("pacers reject bad input", "[server][pacer]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(Pacer(t0, 0, 1), AssertionFailed);
    CHECK_THROWS_AS(Pacer(t0, 61, 1), AssertionFailed);
    CHECK_THROWS_AS(Pacer(t0, 20, 0), AssertionFailed);

    Pacer pacer{t0, 20, 1};
    CHECK_THROWS_AS(pacer.set_rate(0), AssertionFailed);
}
