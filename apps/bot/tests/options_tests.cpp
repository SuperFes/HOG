#include <hog/bot/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string_view>

using namespace std::chrono_literals;
using namespace hog::bot;

namespace {

template <std::size_t N>
using Arguments = std::array<std::string_view, N>;

} // namespace

TEST_CASE("bot options need a server and default to one bot", "[bot][options]") {
    CHECK_FALSE(parse_options({}));

    const auto options = parse_options(Arguments<2>{"--connect", "127.0.0.1:7675"});
    REQUIRE(options);
    CHECK(options->connect == "127.0.0.1:7675");
    CHECK(options->planet == "config/planet.toml");
    CHECK_FALSE(options->login);
    CHECK(options->name == "bot");
    CHECK(options->count == 1);
    CHECK(options->seed == 1);
    CHECK_FALSE(options->run_for);
    CHECK_FALSE(options->help);
}

TEST_CASE("bot options are parsed", "[bot][options]") {
    const auto options = parse_options(Arguments<12>{"--connect", "10.0.0.1:7675", "--login",
                                                     "10.0.0.1:4647", "--name", "walker", "--count",
                                                     "25", "--seed", "9", "--run-for", "120"});
    REQUIRE(options);
    CHECK(options->login == "10.0.0.1:4647");
    CHECK(options->name == "walker");
    CHECK(options->count == 25);
    CHECK(options->seed == 9);
    CHECK(options->run_for == 120s);
}

TEST_CASE("bad bot options are rejected", "[bot][options]") {
    CHECK_FALSE(parse_options(Arguments<2>{"--count", "0"}));
    CHECK_FALSE(parse_options(Arguments<2>{"--count", "101"}));
    CHECK_FALSE(parse_options(Arguments<2>{"--run-for", "soon"}));
    CHECK_FALSE(parse_options(Arguments<1>{"--connect"}));
    CHECK_FALSE(parse_options(Arguments<2>{"--nonsense", "yes"}));
}

TEST_CASE("asking for help doesn't need a server", "[bot][options]") {
    const auto options = parse_options(Arguments<1>{"--help"});
    REQUIRE(options);
    CHECK(options->help);
}
