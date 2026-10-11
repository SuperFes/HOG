#pragma once

#include <hog/core/log.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hog::metrics {

// Shaped after Prometheus (counters, gauges, cumulative histograms) so an exporter can serve
// these as they are. Every metric is safe to update from any thread.

// A total that only grows.
class Counter {
public:
    void add(std::uint64_t amount = 1) noexcept {
        value_.fetch_add(amount, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

// A value that goes up and down.
class Gauge {
public:
    void set(double value) noexcept { value_.store(value, std::memory_order_relaxed); }
    [[nodiscard]] double value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

// Observations counted into buckets by upper bound, with an implicit last bucket for anything
// above the highest bound.
class Histogram {
public:
    struct Snapshot {
        std::vector<double> bounds;
        // Per bucket, not cumulative; one more than there are bounds.
        std::vector<std::uint64_t> counts;
        std::uint64_t count = 0;
        double sum = 0.0;

        // What was observed between `earlier` and this. Precondition: same bounds.
        [[nodiscard]] Snapshot since(const Snapshot& earlier) const;
        friend bool operator==(const Snapshot&, const Snapshot&) = default;
    };

    // Precondition: bounds are finite and strictly ascending.
    explicit Histogram(std::vector<double> bounds);

    void observe(double value);
    [[nodiscard]] Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    Snapshot state_;
};

// Estimates the q-quantile by interpolating within its bucket, as Prometheus's
// histogram_quantile does. NaN without observations; the highest bound for anything above it.
// Precondition: 0 <= q <= 1.
[[nodiscard]] double quantile(const Histogram::Snapshot& snapshot, double q);

// Owns named metrics. Names follow Prometheus: [a-z_][a-z0-9_]*, with counters ending in _total
// and units spelled out (hog_tick_seconds).
class Registry {
public:
    struct Entry {
        std::string name;
        std::string help;
        std::variant<std::unique_ptr<Counter>, std::unique_ptr<Gauge>, std::unique_ptr<Histogram>>
            metric;
    };

    // Preconditions: the name is valid and not already registered.
    Counter& counter(std::string name, std::string help);
    Gauge& gauge(std::string name, std::string help);
    Histogram& histogram(std::string name, std::string help, std::vector<double> bounds);

    // In the order registered. Register everything before the registry is shared with other
    // threads; the metrics themselves can then be updated anywhere.
    [[nodiscard]] std::span<const Entry> entries() const noexcept { return entries_; }

private:
    void add(Entry entry);

    std::vector<Entry> entries_;
};

// Logs every metric as one record of fields: counters and gauges as they stand, and for each
// histogram what was observed since the previous report (_count, _mean, _p50, _p90, _p99).
class Reporter {
public:
    // The registry must outlive the reporter.
    explicit Reporter(const Registry& registry) noexcept : registry_(&registry) {}

    // Writes the record if the category logs at `level`.
    void report(const log::Category& category, log::Level level = log::Level::Info);

private:
    const Registry* registry_;
    // By entry; empty for entries that aren't histograms.
    std::vector<Histogram::Snapshot> previous_;
};

} // namespace hog::metrics
