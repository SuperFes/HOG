#include <hog/app/logging.hpp>
#include <hog/core/line_sink.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>

namespace hog::app {

namespace {

constexpr std::array<const log::Category*, 4> all{&server_log, &save_log, &metrics_log,
                                                  &wildlife_log};

[[nodiscard]] const log::Category* find(std::string_view name) {
    for (const log::Category* category : all) {
        if (category->name() == name) {
            return category;
        }
    }
    return nullptr;
}

} // namespace

std::span<const log::Category* const> categories() noexcept {
    return all;
}

Result<Logging> Logging::start(const server::LogConfig& config, std::ostream& console) {
    for (const auto& [name, level] : config.categories) {
        if (find(name) == nullptr) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("log.categories.{}: no such category", name));
        }
    }
    std::unique_ptr<log::LineSink> file;
    if (config.file) {
        auto opened = log::LineSink::open(*config.file, log::LineFormat::Json);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        file = std::move(*opened);
    }

    for (const log::Category* category : all) {
        category->set_threshold(config.level);
    }
    for (const auto& [name, level] : config.categories) {
        find(name)->set_threshold(level);
    }
    std::vector<log::SinkHandle> sinks;
    sinks.push_back(log::add_sink(std::make_unique<log::LineSink>(console, log::LineFormat::Text)));
    if (file) {
        sinks.push_back(log::add_sink(std::move(file)));
    }
    return Logging{std::move(sinks)};
}

Logging::~Logging() {
    for (const log::SinkHandle sink : sinks_) {
        log::remove_sink(sink);
    }
}

} // namespace hog::app
