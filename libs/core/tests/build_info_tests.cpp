#include <hog/core/build_info.hpp>
#include <hog/core/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>

TEST_CASE("version text matches the numeric version", "[core][build_info]") {
    const auto expected =
        std::format("{}.{}.{}", hog::version.major, hog::version.minor, hog::version.patch);
    CHECK(hog::version_text == expected);
}

TEST_CASE("build info describes this binary", "[core][build_info]") {
    const auto info = hog::build_info();
    CHECK(info.version == hog::version_text);
    CHECK_FALSE(info.compiler.empty());
    CHECK_FALSE(info.config.empty());
}
