#pragma once

#include <hog/core/result.hpp>

#include <filesystem>
#include <string>

namespace hog {

// The whole file. Fails with NotFound when it doesn't exist, Io when it can't be read.
[[nodiscard]] Result<std::string> read_text_file(const std::filesystem::path& path);

} // namespace hog
