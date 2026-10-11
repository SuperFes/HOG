#include <hog/core/assert.hpp>
#include <hog/core/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace hog::metrics {

namespace {

[[nodiscard]] bool valid_name(std::string_view name) {
    const auto lower = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return !name.empty() && lower(name.front()) &&
           std::ranges::all_of(name, [&](char c) { return lower(c) || digit(c); });
}

} // namespace

Histogram::Snapshot Histogram::Snapshot::since(const Snapshot& earlier) const {
    HOG_ASSERT(bounds == earlier.bounds, "snapshots of different histograms");
    Snapshot delta{.bounds = bounds,
                   .counts = counts,
                   .count = count - earlier.count,
                   .sum = sum - earlier.sum};
    for (std::size_t i = 0; i < counts.size(); ++i) {
        delta.counts[i] -= earlier.counts[i];
    }
    return delta;
}

Histogram::Histogram(std::vector<double> bounds) {
    HOG_ASSERT(std::ranges::all_of(bounds, [](double bound) { return std::isfinite(bound); }) &&
                   std::ranges::adjacent_find(bounds, std::greater_equal{}) == bounds.end(),
               "histogram bounds must be finite and strictly ascending");
    state_.counts.assign(bounds.size() + 1, 0);
    state_.bounds = std::move(bounds);
}

void Histogram::observe(double value) {
    const auto bucket = static_cast<std::size_t>(std::ranges::lower_bound(state_.bounds, value) -
                                                 state_.bounds.begin());
    const std::scoped_lock lock{mutex_};
    ++state_.counts[bucket];
    ++state_.count;
    state_.sum += value;
}

Histogram::Snapshot Histogram::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return state_;
}

double quantile(const Histogram::Snapshot& snapshot, double q) {
    HOG_ASSERT(q >= 0.0 && q <= 1.0, "quantiles lie in [0, 1]");
    if (snapshot.count == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double rank = q * static_cast<double>(snapshot.count);
    std::uint64_t below = 0;
    for (std::size_t i = 0; i < snapshot.bounds.size(); ++i) {
        const std::uint64_t in_bucket = snapshot.counts[i];
        if (in_bucket > 0 && static_cast<double>(below + in_bucket) >= rank) {
            const double lower =
                i == 0 ? std::min(0.0, snapshot.bounds[0]) : snapshot.bounds[i - 1];
            const double fraction =
                (rank - static_cast<double>(below)) / static_cast<double>(in_bucket);
            return lower + ((snapshot.bounds[i] - lower) * fraction);
        }
        below += in_bucket;
    }
    return snapshot.bounds.empty() ? std::numeric_limits<double>::quiet_NaN()
                                   : snapshot.bounds.back();
}

void Registry::add(Entry entry) {
    HOG_ASSERT(valid_name(entry.name), "metric names are [a-z_][a-z0-9_]*");
    HOG_ASSERT(std::ranges::none_of(entries_,
                                    [&](const Entry& other) { return other.name == entry.name; }),
               "metric names are unique");
    entries_.push_back(std::move(entry));
}

Counter& Registry::counter(std::string name, std::string help) {
    auto metric = std::make_unique<Counter>();
    Counter& added = *metric;
    add(Entry{.name = std::move(name), .help = std::move(help), .metric = std::move(metric)});
    return added;
}

Gauge& Registry::gauge(std::string name, std::string help) {
    auto metric = std::make_unique<Gauge>();
    Gauge& added = *metric;
    add(Entry{.name = std::move(name), .help = std::move(help), .metric = std::move(metric)});
    return added;
}

Histogram& Registry::histogram(std::string name, std::string help, std::vector<double> bounds) {
    auto metric = std::make_unique<Histogram>(std::move(bounds));
    Histogram& added = *metric;
    add(Entry{.name = std::move(name), .help = std::move(help), .metric = std::move(metric)});
    return added;
}

void Reporter::report(const log::Category& category, log::Level level) {
    if (!category.enabled(level)) {
        return;
    }
    const std::span<const Registry::Entry> entries = registry_->entries();
    previous_.resize(entries.size());

    // Fields only view their names, so everything is gathered before any field is made.
    std::vector<std::pair<std::string, log::FieldValue>> values;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Registry::Entry& entry = entries[i];
        if (const auto* counter = std::get_if<std::unique_ptr<Counter>>(&entry.metric)) {
            values.emplace_back(entry.name, (*counter)->value());
        } else if (const auto* gauge = std::get_if<std::unique_ptr<Gauge>>(&entry.metric)) {
            values.emplace_back(entry.name, (*gauge)->value());
        } else {
            const auto& histogram = std::get<std::unique_ptr<Histogram>>(entry.metric);
            Histogram::Snapshot now = histogram->snapshot();
            if (previous_[i].counts.empty()) {
                previous_[i] =
                    Histogram::Snapshot{.bounds = now.bounds,
                                        .counts = std::vector<std::uint64_t>(now.counts.size(), 0),
                                        .count = 0,
                                        .sum = 0.0};
            }
            const Histogram::Snapshot interval = now.since(previous_[i]);
            previous_[i] = std::move(now);
            const double mean = interval.count == 0
                                    ? std::numeric_limits<double>::quiet_NaN()
                                    : interval.sum / static_cast<double>(interval.count);
            values.emplace_back(entry.name + "_count", interval.count);
            values.emplace_back(entry.name + "_mean", mean);
            values.emplace_back(entry.name + "_p50", quantile(interval, 0.5));
            values.emplace_back(entry.name + "_p90", quantile(interval, 0.9));
            values.emplace_back(entry.name + "_p99", quantile(interval, 0.99));
        }
    }

    std::vector<log::Field> fields;
    fields.reserve(values.size());
    for (const auto& [name, value] : values) {
        fields.push_back(log::Field{.name = name, .value = value});
    }
    log::write(level, category, "metrics", fields);
}

} // namespace hog::metrics
