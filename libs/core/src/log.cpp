#include <hog/core/log.hpp>
#include <hog/core/slot_map.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <iterator>
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

std::string timestamp(std::chrono::system_clock::time_point time) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(time));
}

// Quotes text as a JSON string; the text log uses the same quoting so values stay unambiguous.
void append_quoted(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void append_value(std::string& out, const FieldValue& value) {
    std::visit(
        [&out]<typename T>(const T& held) {
            if constexpr (std::same_as<T, std::string_view>) {
                append_quoted(out, held);
            } else if constexpr (std::same_as<T, double>) {
                if (std::isfinite(held)) {
                    std::format_to(std::back_inserter(out), "{}", held);
                } else {
                    out += "null";
                }
            } else {
                std::format_to(std::back_inserter(out), "{}", held);
            }
        },
        value);
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

std::optional<Level> parse_level(std::string_view name) noexcept {
    constexpr std::array names{"trace", "debug", "info", "warn", "error", "off"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (name == names.at(i)) {
            return static_cast<Level>(i);
        }
    }
    return std::nullopt;
}

std::string format_line(const Record& record) {
    std::string line = std::format("{} {:<5} [{}] {}", timestamp(record.time),
                                   to_string(record.level), record.category, record.message);
    for (const Field& field : record.fields) {
        std::format_to(std::back_inserter(line), " {}=", field.name);
        append_value(line, field.value);
    }
    return line;
}

std::string format_json(const Record& record) {
    std::string json = R"({"time":)";
    append_quoted(json, timestamp(record.time));
    json += R"(,"level":)";
    append_quoted(json, to_string(record.level));
    json += R"(,"category":)";
    append_quoted(json, record.category);
    json += R"(,"message":)";
    append_quoted(json, record.message);
    if (!record.fields.empty()) {
        json += R"(,"fields":{)";
        bool first = true;
        for (const Field& field : record.fields) {
            if (!first) {
                json += ',';
            }
            first = false;
            append_quoted(json, field.name);
            json += ':';
            append_value(json, field.value);
        }
        json += '}';
    }
    json += '}';
    return json;
}

SinkHandle add_sink(std::unique_ptr<Sink> sink) {
    return logger().add(std::move(sink));
}

std::unique_ptr<Sink> remove_sink(SinkHandle handle) {
    return logger().remove(handle);
}

void write(Level level, const Category& category, std::string_view message,
           std::span<const Field> fields) {
    logger().write(Record{.level = level,
                          .category = category.name(),
                          .message = message,
                          .time = std::chrono::system_clock::now(),
                          .fields = fields});
}

} // namespace hog::log
