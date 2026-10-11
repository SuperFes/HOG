#include <hog/core/line_sink.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using hog::log::LineFormat;
using hog::log::LineSink;

namespace {

hog::log::Record record(std::string_view message) {
    using namespace std::chrono;
    return hog::log::Record{.level = hog::log::Level::Warn,
                            .category = "net",
                            .message = message,
                            .time = sys_days{2026y / October / 11}};
}

std::string contents(const std::filesystem::path& path) {
    const std::ifstream file{path};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("a line sink writes one line per record", "[core][log]") {
    std::ostringstream out;
    SECTION("as text") {
        LineSink sink{out, LineFormat::Text};
        sink.write(record("one"));
        sink.write(record("two"));
        CHECK(out.str() == "2026-10-11T00:00:00.000Z WARN  [net] one\n"
                           "2026-10-11T00:00:00.000Z WARN  [net] two\n");
    }
    SECTION("as JSON") {
        LineSink sink{out, LineFormat::Json};
        sink.write(record("one"));
        CHECK(out.str() == hog::log::format_json(record("one")) + "\n");
    }
}

TEST_CASE("a file sink appends, creating its directory", "[core][log]") {
    const hog::testing::TemporaryDirectory directory;
    const auto path = directory.path() / "logs" / "server.jsonl";
    for (const char* message : {"first", "second"}) {
        const auto sink = LineSink::open(path, LineFormat::Json);
        REQUIRE(sink);
        (*sink)->write(record(message));
    }
    CHECK(contents(path) == hog::log::format_json(record("first")) + "\n" +
                                hog::log::format_json(record("second")) + "\n");
}

TEST_CASE("a file sink reports paths it cannot open", "[core][log]") {
    const hog::testing::TemporaryDirectory directory;
    const auto opened = LineSink::open(directory.path(), LineFormat::Text);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == hog::ErrorCode::Io);

    // A directory can't be made under a file.
    std::ofstream{directory.path() / "file"} << "x";
    const auto blocked = LineSink::open(directory.path() / "file" / "log.txt", LineFormat::Text);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == hog::ErrorCode::Io);
}
