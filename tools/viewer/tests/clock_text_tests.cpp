#include <hog/viewer/clock_text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::viewer::drift_text;

TEST_CASE("drift from real time reads in its two largest units", "[viewer][clock]") {
    const GameTime real{std::chrono::days{20'000}};
    CHECK(drift_text(real, real) == "on real time");
    CHECK(drift_text(real + 500ms, real) == "on real time");
    CHECK(drift_text(real - 4s, real) == "on real time");
    CHECK(drift_text(real + 42s, real) == "42s ahead");
    CHECK(drift_text(real - 42s, real) == "42s behind");
    CHECK(drift_text(real + 12min + 5s, real) == "12m 5s ahead");
    CHECK(drift_text(real - 3h - 7min - 59s, real) == "3h 7m behind");
    CHECK(drift_text(real + std::chrono::days{400} + 5h + 1min, real) == "400d 5h ahead");
}

TEST_CASE("speeds run from real time upward", "[viewer][clock]") {
    CHECK(hog::viewer::speeds.front().scale == 1);
    CHECK(std::ranges::is_sorted(hog::viewer::speeds, {}, &hog::viewer::Speed::scale));
}
