#include <hog/client/picture.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <vector>

using hog::client::lift_ink;
using hog::client::load_picture;
using hog::client::Picture;
using hog::client::save_picture;
using Texel = std::vector<int>;

namespace {

Picture of(std::initializer_list<Texel> texels) {
    Picture picture{.width = static_cast<std::uint32_t>(texels.size()), .height = 1, .texels = {}};
    for (const Texel& texel : texels) {
        for (const int value : texel) {
            picture.texels.push_back(static_cast<std::byte>(value));
        }
    }
    return picture;
}

Texel texel(const Picture& picture, std::size_t at) {
    Texel values;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        values.push_back(std::to_integer<int>(picture.texels.at((at * 4) + channel)));
    }
    return values;
}

} // namespace

TEST_CASE("lifting ink off paper clears the paper and keeps the ink", "[client][picture]") {
    Picture picture = of({{255, 255, 255, 255},
                          {0, 0, 0, 255},
                          {255, 0, 0, 255},
                          {128, 128, 128, 255},
                          {255, 128, 128, 255},
                          {0, 0, 0, 0}});
    lift_ink(picture);
    CHECK(texel(picture, 0)[3] == 0);
    CHECK(texel(picture, 1) == Texel{0, 0, 0, 255});
    CHECK(texel(picture, 2) == Texel{255, 0, 0, 255});
    // Half way to white from black is black, half see-through.
    CHECK(texel(picture, 3) == Texel{0, 0, 0, 127});
    CHECK(texel(picture, 4) == Texel{255, 0, 0, 127});
    // What was already clear stays so.
    CHECK(texel(picture, 5)[3] == 0);
}

TEST_CASE("pictures load as sRGB texels with alpha", "[client][picture]") {
    const auto picture = load_picture(std::filesystem::path{HOG_SOURCE_DIR} / "res" / "Matt.png");
    REQUIRE(picture);
    CHECK(picture->width > 0);
    CHECK(picture->height > 0);
    CHECK(picture->texels.size() == std::size_t{picture->width} * picture->height * 4);
    CHECK(!load_picture(std::filesystem::path{HOG_SOURCE_DIR} / "res" / "missing.png"));
}

TEST_CASE("pictures save as PNGs and load back, made opaque", "[client][picture]") {
    const hog::testing::TemporaryDirectory directory;
    const Picture picture = of({{10, 20, 30, 255}, {200, 100, 0, 40}});
    REQUIRE(save_picture(directory.path() / "saved.png", picture));
    const auto loaded = load_picture(directory.path() / "saved.png");
    REQUIRE(loaded);
    CHECK(loaded->width == 2);
    CHECK(loaded->height == 1);
    CHECK(texel(*loaded, 0) == Texel{10, 20, 30, 255});
    CHECK(texel(*loaded, 1) == Texel{200, 100, 0, 255});
    CHECK(!save_picture(directory.path() / "missing" / "saved.png", picture));
}
