#include <hog/core/text_file.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

TEST_CASE("text files are read whole", "[core][file]") {
    const hog::testing::TemporaryDirectory directory;
    const auto path = directory.path() / "a.toml";
    std::ofstream{path, std::ios::binary} << "one\r\ntwo\n";
    CHECK(hog::read_text_file(path) == "one\r\ntwo\n");

    const auto missing = hog::read_text_file(directory.path() / "missing.toml");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == hog::ErrorCode::NotFound);
    CHECK(missing.error().message.starts_with("cannot open "));
}
