#include <hog/app/logging.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

using hog::app::Logging;
using hog::log::Level;

namespace {

std::string contents(const std::filesystem::path& path) {
    const std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("logging sets levels and sinks for its lifetime", "[app][logging]") {
    const hog::testing::TemporaryDirectory directory;
    const auto file = directory.path() / "logs" / "server.jsonl";
    std::ostringstream console;
    {
        const auto logging = Logging::start(
            hog::server::LogConfig{
                .level = Level::Warn, .categories = {{"save", Level::Debug}}, .file = file},
            console);
        REQUIRE(logging);
        CHECK(hog::app::server_log.threshold() == Level::Warn);
        CHECK(hog::app::metrics_log.threshold() == Level::Warn);
        CHECK(hog::app::save_log.threshold() == Level::Debug);

        hog::log::info(hog::app::server_log, "hidden");
        hog::log::debug(hog::app::save_log, "shown");
    }
    hog::log::warn(hog::app::server_log, "after");

    CHECK(console.str().ends_with("DEBUG [save] shown\n"));
    CHECK(console.str().find("hidden") == std::string::npos);
    CHECK(console.str().find("after") == std::string::npos);
    const std::string json = contents(file);
    CHECK(json.starts_with(R"({"time":")"));
    CHECK(json.ends_with(R"(","level":"DEBUG","category":"save","message":"shown"})"
                         "\n"));
}

TEST_CASE("logging fails without changing anything", "[app][logging]") {
    hog::app::server_log.set_threshold(Level::Info);
    std::ostringstream console;

    const auto unknown = Logging::start(
        hog::server::LogConfig{
            .level = Level::Error, .categories = {{"nope", Level::Debug}}, .file = {}},
        console);
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().message == "log.categories.nope: no such category");

    const hog::testing::TemporaryDirectory directory;
    const auto unwritable = Logging::start(
        hog::server::LogConfig{.level = Level::Error, .categories = {}, .file = directory.path()},
        console);
    REQUIRE_FALSE(unwritable);

    CHECK(hog::app::server_log.threshold() == Level::Info);
    CHECK(hog::app::categories().size() == 4);
}
