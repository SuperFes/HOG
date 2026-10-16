#include <hog/net/simulation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

using namespace hog;
using namespace hog::net;
using namespace std::chrono_literals;

TEST_CASE("a simulation is read from its settings, any of them left out") {
    CHECK(parse_simulation("")->off());
    CHECK(*parse_simulation("lag=150,jitter=20,loss=5,reorder=1") ==
          Simulation{.lag = 150ms, .jitter = 20ms, .loss = 5, .reorder = 1});
    CHECK(*parse_simulation("loss=100") == Simulation{.loss = 100});
    CHECK(*parse_simulation("jitter=5,lag=0") == Simulation{.jitter = 5ms});
}

TEST_CASE("a simulation reads back as it's described") {
    for (const std::string_view text : {"", "lag=150", "lag=150,jitter=20,loss=5,reorder=1"}) {
        CAPTURE(text);
        CHECK(describe(*parse_simulation(text)) == text);
    }
}

TEST_CASE("settings the library can't simulate are refused") {
    for (const std::string_view text :
         {"lag", "lag=", "lag=-1", "lag=5001", "jitter=2001", "loss=101", "reorder=x", "lag=1,",
          "latency=10", "lag=1;loss=2"}) {
        CAPTURE(text);
        const auto simulation = parse_simulation(text);
        REQUIRE_FALSE(simulation);
        CHECK(simulation.error().code == ErrorCode::InvalidArgument);
    }
}
