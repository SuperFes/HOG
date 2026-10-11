#pragma once

#include <hog/core/log.hpp>
#include <hog/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>

namespace hog::log {

enum class LineFormat : std::uint8_t {
    Text, // format_line, for people
    Json, // format_json, one object per line, for tools
};

// Writes each record as one line, flushed so a crash loses nothing already logged.
class LineSink final : public Sink {
    // Lets open() construct through make_unique while nothing else can.
    struct Opened {
        explicit Opened() = default;
    };

public:
    // Writes to a stream it doesn't own, such as std::clog. The stream must outlive the sink.
    LineSink(std::ostream& out, LineFormat format) noexcept;

    // Appends to a file, creating it and its directories as needed.
    [[nodiscard]] static Result<std::unique_ptr<LineSink>> open(const std::filesystem::path& path,
                                                                LineFormat format);

    LineSink(Opened /*unused*/, std::unique_ptr<std::ofstream> file, LineFormat format) noexcept;

    void write(const Record& record) override;

private:
    std::unique_ptr<std::ofstream> file_;
    std::ostream* out_;
    LineFormat format_;
};

} // namespace hog::log
