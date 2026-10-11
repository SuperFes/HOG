#pragma once

#include <hog/core/log.hpp>
#include <hog/core/result.hpp>
#include <hog/server/server_config.hpp>

#include <ostream>
#include <span>
#include <vector>

namespace hog::app {

inline const log::Category server_log{"server"};
inline const log::Category save_log{"save"};
inline const log::Category metrics_log{"metrics"};

// The categories a server config can set levels for.
[[nodiscard]] std::span<const log::Category* const> categories() noexcept;

// Sends logs to the console as text and, if configured, to a file as JSON lines, for as long as
// it lives.
class Logging {
public:
    // Sets every category to the config's level, then applies its overrides. Fails, changing
    // nothing, on an unknown category or a file that can't be opened.
    [[nodiscard]] static Result<Logging> start(const server::LogConfig& config,
                                               std::ostream& console);

    ~Logging();
    Logging(Logging&& other) noexcept = default;
    Logging(const Logging&) = delete;
    Logging& operator=(const Logging&) = delete;
    Logging& operator=(Logging&&) = delete;

private:
    explicit Logging(std::vector<log::SinkHandle> sinks) noexcept : sinks_(std::move(sinks)) {}

    std::vector<log::SinkHandle> sinks_;
};

} // namespace hog::app
