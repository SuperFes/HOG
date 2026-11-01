#include <hog/core/image.hpp>

#include <stb_image_write.h>

#include <cstddef>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <vector>

namespace hog {

Result<void> write_png(const std::filesystem::path& path, const Image& image) {
    std::vector<char> encoded;
    const auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<char>*>(context);
        const std::span bytes{static_cast<const char*>(data), static_cast<std::size_t>(size)};
        out->insert(out->end(), bytes.begin(), bytes.end());
    };
    if (stbi_write_png_to_func(append, &encoded, image.width, image.height, 3, image.pixels.data(),
                               image.width * 3) == 0) {
        return make_error(ErrorCode::Internal, std::format("encoding {}", path.string()));
    }
    std::ofstream file{path, std::ios::binary};
    file.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!file) {
        return make_error(ErrorCode::Io, std::format("writing {}", path.string()));
    }
    return {};
}

} // namespace hog
