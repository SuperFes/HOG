#include <hog/client/picture.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <span>
#include <string>

namespace hog::client {

namespace {

struct ImageDeleter {
    void operator()(stbi_uc* pixels) const noexcept { stbi_image_free(pixels); }
};

constexpr int channels = 4;

} // namespace

Result<Picture> load_picture(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int stored = 0;
    const std::unique_ptr<stbi_uc, ImageDeleter> pixels{
        stbi_load(path.string().c_str(), &width, &height, &stored, channels)};
    if (!pixels) {
        return make_error(ErrorCode::Io,
                          std::format("can't load {}: {}", path.string(), stbi_failure_reason()));
    }
    const auto bytes =
        std::as_bytes(std::span{pixels.get(), static_cast<std::size_t>(width) *
                                                  static_cast<std::size_t>(height) * channels});
    return Picture{.width = static_cast<std::uint32_t>(width),
                   .height = static_cast<std::uint32_t>(height),
                   .texels = {bytes.begin(), bytes.end()}};
}

void lift_ink(Picture& picture) {
    constexpr double white = 255.0;
    for (std::size_t at = 0; at + channels <= picture.texels.size(); at += channels) {
        const auto texel = std::span{picture.texels}.subspan(at, channels);
        const auto value = [&](std::size_t channel) {
            return static_cast<double>(std::to_integer<std::uint8_t>(texel[channel]));
        };
        const double paper = std::min({value(0), value(1), value(2)}) / white;
        const double ink = 1.0 - paper;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double colour = ink > 0.0 ? (value(channel) - (paper * white)) / ink : 0.0;
            texel[channel] = static_cast<std::byte>(std::lround(std::clamp(colour, 0.0, white)));
        }
        texel[3] = static_cast<std::byte>(std::lround(ink * value(3)));
    }
}

} // namespace hog::client
