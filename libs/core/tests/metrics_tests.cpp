#include <hog/core/metrics.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using hog::metrics::Histogram;
using hog::metrics::quantile;
using hog::metrics::Registry;

namespace {

class LineCapture final : public hog::log::Sink {
public:
    explicit LineCapture(std::vector<std::string>& lines) : lines_(&lines) {}
    void write(const hog::log::Record& record) override {
        lines_->push_back(hog::log::format_line(record));
    }

private:
    std::vector<std::string>* lines_;
};

// The lines logged during its lifetime.
class Capture {
public:
    Capture() : handle_(hog::log::add_sink(std::make_unique<LineCapture>(lines_))) {}
    ~Capture() { hog::log::remove_sink(handle_); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    [[nodiscard]] const std::vector<std::string>& lines() const noexcept { return lines_; }

private:
    std::vector<std::string> lines_;
    hog::log::SinkHandle handle_;
};

Histogram::Snapshot with_counts(std::vector<std::uint64_t> counts) {
    std::uint64_t count = 0;
    for (const std::uint64_t in_bucket : counts) {
        count += in_bucket;
    }
    return Histogram::Snapshot{
        .bounds = {1.0, 2.0, 4.0}, .counts = std::move(counts), .count = count, .sum = 0.0};
}

} // namespace

TEST_CASE("counters and gauges hold their values", "[core][metrics]") {
    Registry registry;
    auto& ticks = registry.counter("hog_ticks_total", "Ticks run.");
    auto& entities = registry.gauge("hog_entities", "Live entities.");
    ticks.add();
    ticks.add(4);
    entities.set(12.5);
    CHECK(ticks.value() == 5);
    CHECK(entities.value() == 12.5);
    REQUIRE(registry.entries().size() == 2);
    CHECK(registry.entries()[0].name == "hog_ticks_total");
    CHECK(registry.entries()[1].help == "Live entities.");
}

TEST_CASE("histograms count observations by upper bound", "[core][metrics]") {
    Histogram histogram{{1.0, 2.0, 5.0}};
    for (const double value : {0.5, 1.0, 1.5, 10.0}) {
        histogram.observe(value);
    }
    const Histogram::Snapshot first = histogram.snapshot();
    CHECK(first.counts == std::vector<std::uint64_t>{2, 1, 0, 1});
    CHECK(first.count == 4);
    CHECK(first.sum == 13.0);

    histogram.observe(3.0);
    const Histogram::Snapshot interval = histogram.snapshot().since(first);
    CHECK(interval.counts == std::vector<std::uint64_t>{0, 0, 1, 0});
    CHECK(interval.count == 1);
    CHECK(interval.sum == 3.0);
}

TEST_CASE("quantiles interpolate within their bucket", "[core][metrics]") {
    CHECK(quantile(with_counts({10, 0, 0, 0}), 0.5) == Catch::Approx(0.5));
    const auto spread = with_counts({0, 4, 4, 0});
    CHECK(quantile(spread, 0.0) == Catch::Approx(1.0));
    CHECK(quantile(spread, 0.5) == Catch::Approx(2.0));
    CHECK(quantile(spread, 0.75) == Catch::Approx(3.0));
    CHECK(quantile(spread, 1.0) == Catch::Approx(4.0));
    // Above the highest bound, the bound is all that's known.
    CHECK(quantile(with_counts({0, 0, 0, 3}), 0.5) == 4.0);
    CHECK(std::isnan(quantile(with_counts({0, 0, 0, 0}), 0.5)));
}

TEST_CASE("reports log counters as totals and histograms by interval", "[core][metrics]") {
    Registry registry;
    auto& ticks = registry.counter("hog_ticks_total", "Ticks run.");
    auto& tick_time = registry.histogram("hog_tick_seconds", "Tick time.", {0.01, 0.05});
    hog::metrics::Reporter reporter{registry};
    const hog::log::Category category{"metrics"};
    const Capture capture;

    ticks.add(2);
    tick_time.observe(0.005);
    tick_time.observe(0.005);
    reporter.report(category);
    reporter.report(category);
    reporter.report(category, hog::log::Level::Debug);

    REQUIRE(capture.lines().size() == 2);
    CHECK(capture.lines()[0].ends_with(
        "[metrics] metrics hog_ticks_total=2 hog_tick_seconds_count=2 hog_tick_seconds_mean=0.005 "
        "hog_tick_seconds_p50=0.005 hog_tick_seconds_p90=0.009000000000000001 "
        "hog_tick_seconds_p99=0.0099"));
    CHECK(capture.lines()[1].ends_with(
        "hog_ticks_total=2 hog_tick_seconds_count=0 hog_tick_seconds_mean=null "
        "hog_tick_seconds_p50=null hog_tick_seconds_p90=null hog_tick_seconds_p99=null"));
}

TEST_CASE("metric misuse is caught", "[core][metrics]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    Registry registry;
    registry.counter("hog_ticks_total", "");
    CHECK_THROWS_AS(registry.gauge("hog_ticks_total", ""), AssertionFailed);
    CHECK_THROWS_AS(registry.gauge("Hog", ""), AssertionFailed);
    CHECK_THROWS_AS(registry.gauge("9lives", ""), AssertionFailed);
    CHECK_THROWS_AS(registry.gauge("", ""), AssertionFailed);
    CHECK_THROWS_AS(Histogram({2.0, 1.0}), AssertionFailed);
    CHECK_THROWS_AS(Histogram({1.0, 1.0}), AssertionFailed);
    CHECK_THROWS_AS(static_cast<void>(quantile(Histogram::Snapshot{}, 1.5)), AssertionFailed);
}
