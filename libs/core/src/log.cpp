#include <hog/core/log.hpp>
#include <hog/core/slot_map.hpp>

#include <cstdio>
#include <mutex>
#include <print>

namespace hog::log {

namespace {

class Logger {
public:
    SinkHandle add(std::unique_ptr<Sink> sink) {
        const std::scoped_lock lock(mutex_);
        return sinks_.insert(std::move(sink));
    }

    std::unique_ptr<Sink> remove(SinkHandle handle) {
        const std::scoped_lock lock(mutex_);
        if (!sinks_.contains(handle)) {
            return nullptr;
        }
        auto sink = std::move(sinks_.get(handle));
        sinks_.erase(handle);
        return sink;
    }

    void write(const Record& record) {
        const std::scoped_lock lock(mutex_);
        if (sinks_.empty()) {
            std::println(stderr, "{}", format_line(record));
            return;
        }
        for (const auto& sink : sinks_.values()) {
            sink->write(record);
        }
    }

private:
    std::mutex mutex_;
    SlotMap<std::unique_ptr<Sink>, SinkTag> sinks_;
};

Logger& logger() {
    static Logger instance;
    return instance;
}

} // namespace

std::string_view to_string(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Off:
        return "OFF";
    }
    return "?";
}

std::string format_line(const Record& record) {
    const auto time = std::chrono::floor<std::chrono::milliseconds>(record.time);
    return std::format("{:%FT%T}Z {:<5} [{}] {}", time, to_string(record.level), record.category,
                       record.message);
}

SinkHandle add_sink(std::unique_ptr<Sink> sink) {
    return logger().add(std::move(sink));
}

std::unique_ptr<Sink> remove_sink(SinkHandle handle) {
    return logger().remove(handle);
}

void write(Level level, const Category& category, std::string_view message) {
    logger().write(Record{.level = level,
                          .category = category.name(),
                          .message = message,
                          .time = std::chrono::system_clock::now()});
}

} // namespace hog::log
