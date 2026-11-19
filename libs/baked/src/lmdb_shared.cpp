#include "lmdb_shared.hpp"

#include <format>
#include <random>

namespace hog::baked::detail {

Result<std::filesystem::path> new_scratch_directory() {
    std::error_code failed;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(failed);
    if (failed) {
        return make_error(ErrorCode::Io, failed.message());
    }
    std::random_device entropy;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint64_t name = (std::uint64_t{entropy()} << 32U) | entropy();
        std::filesystem::path directory = temporary / std::format("hog-scratch-{:016x}", name);
        if (std::filesystem::create_directory(directory, failed)) {
            return directory;
        }
        if (failed) {
            return make_error(ErrorCode::Io, failed.message());
        }
    }
    return make_error(ErrorCode::Io, "no new scratch directory");
}

} // namespace hog::baked::detail
