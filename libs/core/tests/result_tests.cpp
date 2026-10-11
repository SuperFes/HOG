#include <hog/core/result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <format>
#include <memory>
#include <string_view>
#include <system_error>

namespace {

hog::Result<int> parse_int(std::string_view text) {
    int value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return hog::make_error(hog::ErrorCode::Parse, std::format("'{}' is not an integer", text));
    }
    return value;
}

hog::Result<int> parse_positive(std::string_view text) {
    return parse_int(text).and_then([](int value) -> hog::Result<int> {
        if (value <= 0) {
            return hog::make_error(hog::ErrorCode::OutOfRange, "must be positive");
        }
        return value;
    });
}

} // namespace

TEST_CASE("a successful result holds its value", "[core][result]") {
    const auto result = parse_int("42");
    REQUIRE(result.has_value());
    CHECK(*result == 42);
}

TEST_CASE("a failed result carries code and message", "[core][result]") {
    const auto result = parse_int("4x");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == hog::ErrorCode::Parse);
    CHECK(result.error().message == "'4x' is not an integer");
}

TEST_CASE("results compose through monadic operations", "[core][result]") {
    CHECK(parse_positive("7").value() == 7);
    CHECK(parse_positive("-3").error().code == hog::ErrorCode::OutOfRange);
    CHECK(parse_positive("nope").error().code == hog::ErrorCode::Parse);
}

TEST_CASE("errors format as code and message", "[core][result]") {
    const hog::Error error{.code = hog::ErrorCode::NotFound, .message = "cell 12"};
    CHECK(std::format("{}", error) == "not found: cell 12");
    CHECK(std::format("[{:>20}]", error) == "[  not found: cell 12]");
}

TEST_CASE("void results report success without a value", "[core][result]") {
    const hog::Result<> ok{};
    const hog::Result<> failed = hog::make_error(hog::ErrorCode::Io, "disk full");
    CHECK(ok.has_value());
    CHECK_FALSE(failed.has_value());
}
