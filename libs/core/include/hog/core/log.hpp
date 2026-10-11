#pragma once

#include <hog/core/handle.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace hog::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

[[nodiscard]] std::string_view to_string(Level level) noexcept;

// A named source of log messages with its own threshold, e.g. `inline const log::Category
// net{"net"};`. Messages below the threshold are dropped before formatting. The threshold is a
// runtime knob, so it stays adjustable on const categories.
class Category {
public:
    explicit constexpr Category(std::string_view name, Level threshold = Level::Info) noexcept
        : name_(name), threshold_(threshold) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] Level threshold() const noexcept {
        return threshold_.load(std::memory_order_relaxed);
    }
    void set_threshold(Level level) const noexcept {
        threshold_.store(level, std::memory_order_relaxed);
    }
    [[nodiscard]] bool enabled(Level level) const noexcept {
        return level != Level::Off && level >= threshold();
    }

private:
    std::string_view name_;
    mutable std::atomic<Level> threshold_;
};

struct Record {
    Level level;
    std::string_view category;
    std::string_view message;
    std::chrono::system_clock::time_point time;
};

// Formats a record as one line: "2026-10-11T03:52:50.123Z INFO  [net] message".
[[nodiscard]] std::string format_line(const Record& record);

// Destinations are open-ended (console, file, test capture, in-game console), hence an interface.
// write() is called under the logger's lock, so sinks need no locking of their own.
class Sink {
public:
    Sink() = default;
    virtual ~Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    virtual void write(const Record& record) = 0;
};

struct SinkTag {};
using SinkHandle = Handle<SinkTag>;

// Without any added sink, records go to stderr.
SinkHandle add_sink(std::unique_ptr<Sink> sink);
std::unique_ptr<Sink> remove_sink(SinkHandle handle);

void write(Level level, const Category& category, std::string_view message);

template <typename... Args>
void message(Level level, const Category& category, std::format_string<Args...> format,
             Args&&... args) {
    if (category.enabled(level)) {
        write(level, category, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void trace(const Category& category, std::format_string<Args...> format, Args&&... args) {
    message(Level::Trace, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(const Category& category, std::format_string<Args...> format, Args&&... args) {
    message(Level::Debug, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void info(const Category& category, std::format_string<Args...> format, Args&&... args) {
    message(Level::Info, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(const Category& category, std::format_string<Args...> format, Args&&... args) {
    message(Level::Warn, category, format, std::forward<Args>(args)...);
}

template <typename... Args>
void error(const Category& category, std::format_string<Args...> format, Args&&... args) {
    message(Level::Error, category, format, std::forward<Args>(args)...);
}

} // namespace hog::log
