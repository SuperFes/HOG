#include <hog/viewer/texels.hpp>

namespace hog::viewer {

std::vector<std::byte> rgba_texels(const mapgen::Image& image) {
    std::vector<std::byte> texels;
    texels.reserve(image.pixels.size() / 3 * 4);
    for (std::size_t at = 0; at + 3 <= image.pixels.size(); at += 3) {
        texels.push_back(static_cast<std::byte>(image.pixels[at]));
        texels.push_back(static_cast<std::byte>(image.pixels[at + 1]));
        texels.push_back(static_cast<std::byte>(image.pixels[at + 2]));
        texels.push_back(std::byte{255});
    }
    return texels;
}

} // namespace hog::viewer
