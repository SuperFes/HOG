#include <hog/client/likeness.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <vector>

namespace hog::client {

namespace {

constexpr std::size_t channels = 4;
// Differences within the limits are a few counts, too dark to see without brightening.
constexpr int brightened = 8;

int value(const Picture& picture, std::size_t at) {
    return std::to_integer<int>(picture.texels[at]);
}

} // namespace

Result<Difference> difference(const Picture& expected, const Picture& drawn,
                              const Likeness& likeness) {
    if (expected.width != drawn.width || expected.height != drawn.height) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("a picture of {}×{} drawn as {}×{}", expected.width,
                                      expected.height, drawn.width, drawn.height));
    }
    const std::size_t pixels = std::size_t{expected.width} * expected.height;
    if (expected.texels.size() != pixels * channels || drawn.texels.size() != pixels * channels) {
        return make_error(ErrorCode::InvalidArgument, "a picture without a texel for each pixel");
    }
    Difference found{.mean = 0.0,
                     .share = 0.0,
                     .picture = {.width = expected.width,
                                 .height = expected.height,
                                 .texels = std::vector<std::byte>(pixels * channels)}};
    std::size_t total = 0;
    std::size_t over = 0;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        int largest = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const std::size_t at = (pixel * channels) + channel;
            largest = std::max(largest, std::abs(value(expected, at) - value(drawn, at)));
        }
        total += static_cast<std::size_t>(largest);
        if (largest > likeness.counts) {
            ++over;
        }
        const auto grey = static_cast<std::byte>(std::min(largest * brightened, 255));
        const auto texel = std::span{found.picture.texels}.subspan(pixel * channels, channels);
        std::fill_n(texel.begin(), 3, grey);
        texel[3] = std::byte{255};
    }
    if (pixels > 0) {
        found.mean = static_cast<double>(total) / static_cast<double>(pixels);
        found.share = static_cast<double>(over) / static_cast<double>(pixels);
    }
    return found;
}

bool alike(const Difference& difference, const Likeness& likeness) noexcept {
    return difference.mean <= likeness.mean && difference.share <= likeness.share;
}

} // namespace hog::client
