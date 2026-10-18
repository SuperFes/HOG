#include <hog/client/chat.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string>

using namespace std::chrono_literals;
using hog::MonotonicClock;
using hog::client::censored;
using hog::client::ChatLog;

TEST_CASE("profanity is hidden behind asterisks, whatever the case", "[client][chat]") {
    CHECK(censored("Fuck off") == "**** off");
    CHECK(censored("FUCK") == "****");
    CHECK(censored("that's bullshit") == "that's bull****");
    CHECK(censored("you ass") == "you ***");
    CHECK(censored("hello there") == "hello there");
}

TEST_CASE("a hidden word's endings go with it", "[client][chat]") {
    CHECK(censored("fucking hell") == "******* hell");
    CHECK(censored("no shits given") == "no ***** given");
}

TEST_CASE("innocent words holding profanity are left be", "[client][chat]") {
    CHECK(censored("the class assignment") == "the class assignment");
    CHECK(censored("Scunthorpe") == "Scunthorpe");
    CHECK(censored("the cockpit canopy") == "the cockpit canopy");
    CHECK(censored("a prickly passage") == "a prickly passage");
    CHECK(censored("he's a prick") == "he's a *****");
}

TEST_CASE("hiding profanity keeps the line's length", "[client][chat]") {
    const std::string line = "what the fuck is this shit";
    CHECK(censored(line).size() == line.size());
}

TEST_CASE("the chat log keeps only so many lines", "[client][chat]") {
    ChatLog log;
    const MonotonicClock::time_point now = MonotonicClock::now();
    for (std::size_t line = 0; line < ChatLog::kept + 10; ++line) {
        log.add("Ada", std::format("line {}", line), now);
    }
    REQUIRE(log.lines().size() == ChatLog::kept);
    CHECK(log.lines().front().text == "line 10");
    CHECK(log.lines().back().text == std::format("line {}", ChatLog::kept + 9));
}

TEST_CASE("only lately heard lines stay over the picture", "[client][chat]") {
    ChatLog log;
    const MonotonicClock::time_point start = MonotonicClock::now();
    log.add("Ada", "long ago", start);
    log.add("Ada", "just now", start + 30s);
    const auto fresh = log.fresh(start + 31s);
    REQUIRE(fresh.size() == 1);
    CHECK(fresh.front().text == "just now");
    CHECK(log.fresh(start + 60s).empty());
}

TEST_CASE("only the newest few lines stay over the picture", "[client][chat]") {
    ChatLog log;
    const MonotonicClock::time_point now = MonotonicClock::now();
    for (std::size_t line = 0; line < ChatLog::most_fresh + 5; ++line) {
        log.add("Ada", std::format("line {}", line), now);
    }
    const auto fresh = log.fresh(now);
    REQUIRE(fresh.size() == ChatLog::most_fresh);
    CHECK(fresh.front().text == "line 5");
}
