#include <hog/core/line_sink.hpp>

#include <format>
#include <system_error>
#include <utility>

namespace hog::log {

LineSink::LineSink(std::ostream& out, LineFormat format) noexcept : out_(&out), format_(format) {}

LineSink::LineSink(Opened /*unused*/, std::unique_ptr<std::ofstream> file,
                   LineFormat format) noexcept
    : file_(std::move(file)), out_(file_.get()), format_(format) {}

Result<std::unique_ptr<LineSink>> LineSink::open(const std::filesystem::path& path,
                                                 LineFormat format) {
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return make_error(
                ErrorCode::Io,
                std::format("cannot create {}: {}", path.parent_path().string(), error.message()));
        }
    }
    auto file = std::make_unique<std::ofstream>(path, std::ios::app | std::ios::binary);
    if (!*file) {
        return make_error(ErrorCode::Io, std::format("cannot open {}", path.string()));
    }
    return std::make_unique<LineSink>(Opened{}, std::move(file), format);
}

void LineSink::write(const Record& record) {
    *out_ << (format_ == LineFormat::Json ? format_json(record) : format_line(record)) << '\n';
    out_->flush();
}

} // namespace hog::log
