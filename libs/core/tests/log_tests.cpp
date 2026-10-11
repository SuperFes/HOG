#include <hog/core/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Captured {
    hog::log::Level level;
    std::string category;
    std::string message;
};

class CaptureSink final : public hog::log::Sink {
public:
    explicit CaptureSink(std::vector<Captured>& out) : out_(&out) {}

    void write(const hog::log::Record& record) override {
        out_->push_back(Captured{.level = record.level,
                                 .category = std::string(record.category),
                                 .message = std::string(record.message)});
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
