#include <hog/login_app/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using hog::ErrorCode;
using namespace hog::login_app;

using Arguments = std::vector<std::string_view>;

TEST_CASE("login options default to the HOGS port and var", "[login]") {
    const auto options = parse_options({});
    REQUIRE(options);
    CHECK(options->port == 4647);
    CHECK(options->keys == "var");
    CHECK_FALSE(options->help);
}

TEST_CASE("login options are parsed", "[login]") {
    const auto options = parse_options(Arguments{"--port", "5000", "--keys", "/srv/keys", "-h"});
    REQUIRE(options);
    CHECK(options->port == 5000);
    CHECK(options->keys == "/srv/keys");
    CHECK(options->help);
}

TEST_CASE("login options refuse what they can't use", "[login]") {
    for (const Arguments& wrong :
         {Arguments{"--port"}, Arguments{"--port", "0"}, Arguments{"--port", "70000"},
          Arguments{"--port", "12x"}, Arguments{"--keys"}, Arguments{"--colour"}}) {
        CHECK(parse_options(wrong).error().code == ErrorCode::InvalidArgument);
    }
}
