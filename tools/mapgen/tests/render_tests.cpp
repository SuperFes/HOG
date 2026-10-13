#include <hog/mapgen/render.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <set>
#include <vector>

using hog::mapgen::FaceView;
using hog::mapgen::ImageSize;
using hog::mapgen::Layer;
using hog::mapgen::PlanetView;
using hog::mapgen::RegionView;
using hog::mapgen::View;
using hog::planet::CellId;
using hog::planet::fixed_one;

namespace {

constexpr double radius = 115'000.0;

double latitude(hog::DVec3 direction) {
    return std::asin(direction.z) * 180.0 / std::numbers::pi;
}

double longitude(hog::DVec3 direction) {
    return std::atan2(direction.y, direction.x) * 180.0 / std::numbers::pi;
}

} // namespace

TEST_CASE("the planet view is twice as wide as it is tall", "[mapgen]") {
    CHECK(image_size(View{PlanetView{}}, 512) == ImageSize{.width = 512, .height = 256});
    CHECK(image_size(View{FaceView{.face = 1}}, 512) == ImageSize{.width = 512, .height = 512});
    CHECK(image_size(View{RegionView{.width = 1.0}}, 64) == ImageSize{.width = 64, .height = 64});
}

TEST_CASE("the planet view runs east across and south down", "[mapgen]") {
    const View view = PlanetView{};
    const ImageSize size = image_size(view, 360);
    const auto at = [&](int x, int y) {
        return hog::mapgen::pixel_direction(view, size, x, y, radius);
    };
    // Pixels are one degree square, centred half a degree in.
    CHECK(latitude(at(0, 0)) == Catch::Approx(89.5));
    CHECK(latitude(at(0, 179)) == Catch::Approx(-89.5));
    CHECK(longitude(at(0, 90)) == Catch::Approx(-179.5));
    CHECK(longitude(at(180, 90)) == Catch::Approx(0.5));
    CHECK(latitude(at(180, 90)) == Catch::Approx(-0.5));
    CHECK(glm::length(at(17, 33)) == Catch::Approx(1.0));
}

TEST_CASE("the face view lines pixels up with the face's cells", "[mapgen]") {
    constexpr int level = 5;
    constexpr int side = 1 << level;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const View view = FaceView{.face = face};
        const ImageSize size = image_size(view, side);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const CellId expected =
                    CellId::from_face_ij(face, static_cast<std::uint32_t>(x),
                                         static_cast<std::uint32_t>(side - 1 - y), level);
                REQUIRE(hog::planet::cell_at(hog::mapgen::pixel_direction(view, size, x, y, radius),
                                             level) == expected);
            }
        }
    }
}

TEST_CASE("the region view is the requested width, north up", "[mapgen]") {
    const View view = RegionView{.latitude = 40.0, .longitude = -70.0, .width = 6'400.0};
    const ImageSize size = image_size(view, 64);
    const auto at = [&](int x, int y) {
        return hog::mapgen::pixel_direction(view, size, x, y, radius);
    };
    const auto metres = [](hog::DVec3 a, hog::DVec3 b) { return glm::length(a - b) * radius; };
    // 100 m pixels.
    CHECK(metres(at(10, 10), at(11, 10)) == Catch::Approx(100.0).epsilon(0.001));
    CHECK(metres(at(10, 10), at(10, 11)) == Catch::Approx(100.0).epsilon(0.001));
    CHECK(latitude(at(0, 0)) > latitude(at(0, 63)));
    CHECK(longitude(at(0, 0)) < longitude(at(63, 0)));
    const hog::DVec3 middle = glm::normalize(at(31, 31) + at(32, 32));
    CHECK(latitude(middle) == Catch::Approx(40.0).margin(1e-9));
    CHECK(longitude(middle) == Catch::Approx(-70.0).margin(1e-9));
}

TEST_CASE("the islands view centres on the starting islands", "[mapgen]") {
    const hog::planet::PlanetConfig planet{.seed = 3, .ocean_fraction = 0.7, .radius = radius};
    const hog::planet::Geology geology{planet};
    const View view =
        hog::mapgen::resolve(View{hog::mapgen::IslandsView{.width = 2'000.0}}, geology);
    REQUIRE(std::holds_alternative<RegionView>(view));
    CHECK(std::get<RegionView>(view).width == 2'000.0);
    const ImageSize size = image_size(view, 2);
    const hog::DVec3 middle =
        glm::normalize(hog::mapgen::pixel_direction(view, size, 0, 0, radius) +
                       hog::mapgen::pixel_direction(view, size, 1, 1, radius));
    CHECK(glm::length(middle - geology.terrain().archipelago().center()) < 1e-9);
    // Other views pass through.
    CHECK(hog::mapgen::resolve(View{FaceView{.face = 3}}, geology) == View{FaceView{.face = 3}});
}

TEST_CASE("height colours run from deep blue to snow", "[mapgen]") {
    const auto colour = [](double metres) {
        return hog::mapgen::height_colour(std::llround(metres * fixed_one));
    };
    const auto brightness = [](hog::mapgen::Rgb rgb) { return rgb[0] + rgb[1] + rgb[2]; };
    for (const double depth : {-6'000.0, -2'000.0, -50.0, 0.0}) {
        const auto rgb = colour(depth);
        CHECK(rgb[2] > rgb[0]);
    }
    CHECK(brightness(colour(-3'000.0)) < brightness(colour(-500.0)));
    const auto lowland = colour(50.0);
    CHECK(lowland[1] > lowland[0]);
    CHECK(lowland[1] > lowland[2]);
    CHECK(colour(9'000.0) == hog::mapgen::Rgb{245, 245, 245});
}

TEST_CASE("every biome has its own colour", "[mapgen]") {
    std::set<hog::mapgen::Rgb> colours;
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        colours.insert(hog::mapgen::biome_colour(static_cast<hog::planet::Biome>(biome)));
    }
    CHECK(colours.size() == hog::planet::biome_count);
}

TEST_CASE("every rock and resource has its own colour", "[mapgen]") {
    std::set<hog::mapgen::Rgb> colours;
    for (int rock = 0; rock < hog::planet::rock_count; ++rock) {
        colours.insert(hog::mapgen::rock_colour(static_cast<hog::planet::Rock>(rock)));
    }
    for (int resource = 0; resource < hog::planet::resource_count; ++resource) {
        colours.insert(hog::mapgen::resource_colour(static_cast<hog::planet::Resource>(resource)));
    }
    CHECK(colours.size() == hog::planet::rock_count + hog::planet::resource_count);
}

TEST_CASE("temperature colours run from blue through white to red", "[mapgen]") {
    const auto colour = [](std::int64_t celsius) {
        return hog::mapgen::temperature_colour(celsius * fixed_one);
    };
    CHECK(colour(-20)[2] > colour(-20)[0]);
    CHECK(colour(0) == hog::mapgen::Rgb{245, 245, 245});
    CHECK(colour(30)[0] > colour(30)[2]);
    CHECK(colour(-60) == colour(-30));
}

TEST_CASE("moisture colours run from brown to blue", "[mapgen]") {
    const auto dry = hog::mapgen::moisture_colour(0);
    const auto wet = hog::mapgen::moisture_colour(fixed_one);
    CHECK(dry[0] > dry[2]);
    CHECK(wet[2] > wet[0]);
    const auto middling = hog::mapgen::moisture_colour(fixed_one * 6 / 10);
    CHECK(middling[1] > middling[0]);
    CHECK(middling[1] > middling[2]);
}

TEST_CASE("water table colours run from teal at the surface to brown far down", "[mapgen]") {
    const auto colour = [](std::int64_t metres) {
        return hog::mapgen::water_table_colour(metres * fixed_one);
    };
    CHECK(colour(0)[2] > colour(0)[0]);
    CHECK(colour(100)[0] > colour(100)[2]);
    CHECK(colour(500) == colour(200));
}

TEST_CASE("lakes and rivers show over the ground", "[mapgen]") {
    const hog::mapgen::Rgb ground{10, 200, 10};
    CHECK(hog::mapgen::with_water(ground, false, nullptr) == ground);
    CHECK(hog::mapgen::with_water(ground, true, nullptr) ==
          hog::mapgen::biome_colour(hog::planet::Biome::Lake));
    hog::planet::Reach narrow;
    narrow.width = 2.0;
    hog::planet::Reach wide;
    wide.width = 40.0;
    const auto creek = hog::mapgen::with_water(ground, false, &narrow);
    const auto river = hog::mapgen::with_water(ground, false, &wide);
    CHECK(creek[2] > creek[0]);
    CHECK(river[2] > river[0]);
    CHECK(river[1] < creek[1]);
}

TEST_CASE("water maps draw rivers wherever they run", "[mapgen]") {
    const hog::planet::PlanetConfig planet{.seed = 1, .ocean_fraction = 0.7, .radius = radius};
    const hog::planet::Geology geology{planet};
    hog::jobs::JobSystem jobs{2};
    const auto image =
        hog::mapgen::render(geology, radius, View{PlanetView{}}, Layer::Water, 512, jobs);
    int river = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto pixel = image.at(x, y);
            river += pixel[2] > 180 && pixel[0] < 100 ? 1 : 0;
        }
    }
    CHECK(river > 100);
}

TEST_CASE("deposit maps show the rarest deposit over grey land", "[mapgen]") {
    using hog::planet::Resource;
    std::array<hog::planet::Deposit, hog::planet::resource_count> deposits{};
    const auto grey = hog::mapgen::deposit_colour(deposits, 100 * fixed_one);
    CHECK(grey[0] == grey[1]);
    CHECK(grey[1] == grey[2]);
    CHECK(hog::mapgen::deposit_colour(deposits, 3'000 * fixed_one)[0] > grey[0]);
    CHECK(hog::mapgen::deposit_colour(deposits, -fixed_one) != grey);

    const auto with = [&](Resource resource) -> hog::planet::Deposit& {
        return deposits.at(static_cast<std::size_t>(resource));
    };
    with(Resource::Iron) = {.top = 0, .thickness = fixed_one};
    const auto iron = hog::mapgen::deposit_colour(deposits, 100 * fixed_one);
    CHECK(iron != grey);
    with(Resource::Gold) = {.top = 0, .thickness = fixed_one};
    const auto gold = hog::mapgen::deposit_colour(deposits, 100 * fixed_one);
    CHECK(gold != iron);
    with(Resource::Iron) = {};
    CHECK(hog::mapgen::deposit_colour(deposits, 100 * fixed_one) == gold);
}

TEST_CASE("every layer renders", "[mapgen]") {
    const hog::planet::PlanetConfig planet{.seed = 1, .ocean_fraction = 0.7, .radius = radius};
    const hog::planet::Geology geology{planet};
    hog::jobs::JobSystem jobs{2};
    std::set<std::vector<std::uint8_t>> images;
    for (int layer = 0; layer < hog::mapgen::layer_count; ++layer) {
        const auto image = hog::mapgen::render(geology, radius, View{PlanetView{}},
                                               static_cast<Layer>(layer), 32, jobs);
        CHECK(image.pixels.size() == std::size_t{32} * 16 * 3);
        images.insert(image.pixels);
    }
    CHECK(images.size() == hog::mapgen::layer_count);
}

TEST_CASE("rendering doesn't depend on the thread count", "[mapgen]") {
    const hog::planet::PlanetConfig planet{.seed = 1, .ocean_fraction = 0.7, .radius = radius};
    const hog::planet::Geology geology{planet};
    hog::jobs::JobSystem one{1};
    const auto single =
        hog::mapgen::render(geology, radius, View{PlanetView{}}, Layer::Height, 64, one);
    CHECK(single.width == 64);
    CHECK(single.height == 32);
    CHECK(single.pixels.size() == std::size_t{64} * 32 * 3);
    // A world of ocean and land both.
    CHECK(single.at(0, 0) != single.at(40, 16));
    hog::jobs::JobSystem many{4};
    CHECK(
        hog::mapgen::render(geology, radius, View{PlanetView{}}, Layer::Height, 64, many).pixels ==
        single.pixels);
}

TEST_CASE("map files are named for their view and layer", "[mapgen]") {
    CHECK(hog::mapgen::file_name(View{PlanetView{}}, Layer::Height) == "planet-height.png");
    CHECK(hog::mapgen::file_name(View{FaceView{.face = 2}}, Layer::Height) == "face2-height.png");
    CHECK(hog::mapgen::file_name(View{RegionView{}}, Layer::Height) == "region-height.png");
    CHECK(hog::mapgen::file_name(View{hog::mapgen::IslandsView{}}, Layer::Biome) ==
          "islands-biome.png");
}

TEST_CASE("maps are written as PNG", "[mapgen]") {
    const hog::mapgen::Image image{
        .width = 3, .height = 2, .pixels = std::vector<std::uint8_t>(18, 7)};
    const auto path = std::filesystem::temp_directory_path() / "hog-mapgen-test.png";
    REQUIRE(hog::mapgen::write_png(path, image));
    std::ifstream file{path, std::ios::binary};
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{file}, {}};
    file.close();
    std::filesystem::remove(path);
    REQUIRE(bytes.size() > 24);
    constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(std::equal(signature.begin(), signature.end(), bytes.begin()));
    // IHDR's big-endian width and height.
    CHECK(bytes.at(19) == 3);
    CHECK(bytes.at(23) == 2);

    CHECK_FALSE(hog::mapgen::write_png(std::filesystem::temp_directory_path(), image));
}
