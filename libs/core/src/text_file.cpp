#include <hog/core/text_file.hpp>

#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace hog {

Result<std::string> read_text_file(const std::filesystem::path& path) {
    const std::ifstream file{path, std::ios::binary};
    if (!file) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        return make_error(exists ? ErrorCode::Io : ErrorCode::NotFound,
                          std::format("cannot open {}", path.string()));
    }
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return make_error(ErrorCode::Io, std::format("cannot read {}", path.string()));
    }
    return text.str();
}

} // namespace hog
