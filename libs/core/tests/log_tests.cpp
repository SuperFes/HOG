#include <hog/core/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Captured {
    hog::log::Level level;
    std::string category;
    std::string message;
    std::string line;
};

class CaptureSink final : public hog::log::Sink {
public:
    explicit CaptureSink(std::vector<Captured>& out) : out_(&out) {}

    void write(const hog::log::Record& record) override {
        out_->push_back(Captured{.level = record.level,
                                 .category = std::string(record.category),
                                 .message = std::string(record.message),
                                 .line = hog::log::format_line(record)});
    }

private:
    std::vector<Captured>* out_;
};

// Captures everything logged during its lifetime.
class Capture {
public:
    Capture() : handle_(hog::log::add_sink(std::make_unique<CaptureSink>(records_))) {}
    ~Capture() { hog::log::remove_sink(handle_); }

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    [[nodiscard]] const std::vector<Captured>& records() const noexcept { return records_; }

private:
    std::vector<Captured> records_;
    hog::log::SinkHandle handle_;
};

} // namespace

TEST_CASE("messages at or above the threshold reach sinks", "[core][log]") {
    const hog::log::Category category{"test", hog::log::Level::Info};
    const Capture capture;

    hog::log::debug(category, "hidden {}", 1);
    hog::log::info(category, "cell {} loaded", 42);
    hog::log::error(category, "disk {}", "full");

    REQUIRE(capture.records().size() == 2);
    CHECK(capture.records()[0].level == hog::log::Level::Info);
    CHECK(capture.records()[0].category == "test");
    CHECK(capture.records()[0].message == "cell 42 loaded");
    CHECK(capture.records()[1].level == hog::log::Level::Error);
}

TEST_CASE("thresholds can change at runtime", "[core][log]") {
    const hog::log::Category category{"test", hog::log::Level::Warn};
    const Capture capture;

    hog::log::info(category, "dropped");
    category.set_threshold(hog::log::Level::Trace);
    hog::log::trace(category, "kept");
    category.set_threshold(hog::log::Level::Off);
    hog::log::error(category, "dropped");

    REQUIRE(capture.records().size() == 1);
    CHECK(capture.records()[0].message == "kept");
}

TEST_CASE("removed sinks stop receiving records", "[core][log]") {
    const hog::log::Category category{"test"};
    std::vector<Captured> records;
    const auto handle = hog::log::add_sink(std::make_unique<CaptureSink>(records));

    hog::log::info(category, "one");
    auto sink = hog::log::remove_sink(handle);
    hog::log::info(category, "two");

    CHECK(sink != nullptr);
    CHECK(records.size() == 1);
    CHECK(hog::log::remove_sink(handle) == nullptr);
}

TEST_CASE("lines carry UTC time, level, category and message", "[core][log]") {
    using namespace std::chrono;
    const auto time = sys_days{2026y / October / 11} + 3h + 52min + 50s + 123ms;
    const hog::log::Record record{
        .level = hog::log::Level::Warn, .category = "net", .message = "lag", .time = time};
    CHECK(hog::log::format_line(record) == "2026-10-11T03:52:50.123Z WARN  [net] lag");
}

namespace {

hog::log::Record at_noon(std::span<const hog::log::Field> fields) {
    using namespace std::chrono;
    return hog::log::Record{.level = hog::log::Level::Info,
                            .category = "server",
                            .message = "saved",
                            .time = sys_days{2026y / October / 11} + 12h,
                            .fields = fields};
}

} // namespace

TEST_CASE("fields follow the message as key=value", "[core][log]") {
    const std::array<hog::log::Field, 5> fields{{
        {.name = "ticks", .value = std::uint64_t{20}},
        {.name = "behind", .value = std::int64_t{-3}},
        {.name = "ms", .value = 1.5},
        {.name = "file", .value = std::string_view{"a \"b\".db"}},
        {.name = "ok", .value = true},
    }};
    CHECK(hog::log::format_line(at_noon(fields)) ==
          R"(2026-10-11T12:00:00.000Z INFO  [server] saved ticks=20 behind=-3 ms=1.5 )"
          R"(file="a \"b\".db" ok=true)");
}

TEST_CASE("records format as one JSON object", "[core][log]") {
    CHECK(hog::log::format_json(at_noon({})) ==
          R"({"time":"2026-10-11T12:00:00.000Z","level":"INFO","category":"server",)"
          R"("message":"saved"})");

    const std::array<hog::log::Field, 4> fields{{
        {.name = "count", .value = std::uint64_t{3}},
        {.name = "lost", .value = std::numeric_limits<double>::infinity()},
        {.name = "path", .value = std::string_view{"C:\\x\n\x01"}},
        {.name = "fine", .value = false},
    }};
    CHECK(hog::log::format_json(at_noon(fields)) ==
          R"({"time":"2026-10-11T12:00:00.000Z","level":"INFO","category":"server",)"
          R"("message":"saved","fields":{"count":3,"lost":null,"path":"C:\\x\n\u0001",)"
          R"("fine":false}})");
}

TEST_CASE("fields reach sinks", "[core][log]") {
    const hog::log::Category category{"test"};
    const Capture capture;
    const std::array<hog::log::Field, 1> fields{{{.name = "n", .value = std::int64_t{7}}}};
    hog::log::write(hog::log::Level::Info, category, "counted", fields);
    REQUIRE(capture.records().size() == 1);
    CHECK(capture.records()[0].line.ends_with("[test] counted n=7"));
}

TEST_CASE("levels parse from their lowercase names", "[core][log]") {
    using hog::log::Level;
    for (const Level level :
         {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Off}) {
        std::string name{hog::log::to_string(level)};
        for (char& c : name) {
            c = static_cast<char>(c - 'A' + 'a');
        }
        CHECK(hog::log::parse_level(name) == level);
    }
    CHECK_FALSE(hog::log::parse_level("INFO"));
    CHECK_FALSE(hog::log::parse_level("verbose"));
    CHECK_FALSE(hog::log::parse_level(""));
}
