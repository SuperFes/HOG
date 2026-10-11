#pragma once

#include <string_view>

namespace hog {

// Identifies the running binary in logs and crash reports.
struct BuildInfo {
    std::string_view version;
    std::string_view compiler;
    std::string_view config;
};

[[nodiscard]] BuildInfo build_info() noexcept;

} // namespace hog
