#include <hog/mapgen/render.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <numbers>
#include <span>

namespace hog::mapgen {

namespace {

struct Stop {
    double at; // the value where the colour is exact
    Rgb colour;
};

constexpr std::array ocean_stops{
    Stop{.at = -4'000.0, .colour = {8, 30, 80}},
    Stop{.at = -1'000.0, .colour = {25, 70, 140}},
    Stop{.at = -100.0, .colour = {60, 130, 195}},
    Stop{.at = 0.0, .colour = {130, 190, 225}},
};

constexpr std::array land_stops{
    Stop{.at = 0.0, .colour = {70, 140, 70}},       Stop{.at = 300.0, .colour = {150, 180, 95}},
    Stop{.at = 1'000.0, .colour = {195, 165, 110}}, Stop{.at = 2'500.0, .colour = {125, 95, 75}},
    Stop{.at = 4'000.0, .colour = {245, 245, 245}},
};

template <std::size_t N>
[[nodiscard]] Rgb interpolate(const std::array<Stop, N>& stops, double value) {
    if (value <= stops.front().at) {
        return stops.front().colour;
    }
    for (std::size_t index = 1; index < N; ++index) {
        const Stop& high = stops.at(index);
        if (value <= high.at) {
            const Stop& low = stops.at(index - 1);
            const double t = (value - low.at) / (high.at - low.at);
            Rgb colour{};
            for (std::size_t channel = 0; channel < colour.size(); ++channel) {
                const double mixed = low.colour.at(channel) +
                                     (t * (high.colour.at(channel) - low.colour.at(channel)));
                colour.at(channel) = static_cast<std::uint8_t>(std::lround(mixed));
            }
            return colour;
        }
    }
    return stops.back().colour;
}

constexpr std::array temperature_stops{
    Stop{.at = -30.0, .colour = {60, 20, 120}}, Stop{.at = -10.0, .colour = {60, 110, 220}},
    Stop{.at = 0.0, .colour = {245, 245, 245}}, Stop{.at = 15.0, .colour = {245, 200, 80}},
    Stop{.at = 35.0, .colour = {190, 30, 30}},
};

constexpr std::array moisture_stops{
    Stop{.at = 0.0, .colour = {150, 100, 50}},
    Stop{.at = 0.3, .colour = {225, 205, 120}},
    Stop{.at = 0.6, .colour = {80, 170, 80}},
    Stop{.at = 1.0, .colour = {30, 80, 190}},
};

// Indexed by biome.
constexpr std::array<Rgb, planet::biome_count> biome_colours{{
    {20, 50, 110},   // ocean
    {230, 215, 160}, // beach
    {240, 248, 255}, // ice
    {170, 175, 150}, // tundra
    {40, 90, 60},    // boreal forest
    {60, 135, 60},   // temperate forest
    {150, 195, 90},  // grassland
    {175, 160, 100}, // shrubland
    {230, 200, 125}, // desert
    {200, 185, 80},  // savanna
    {10, 95, 35},    // tropical rainforest
    {145, 135, 135}, // alpine
}};

// Indexed by resource.
constexpr std::array<Rgb, planet::resource_count> resource_colours{{
    {205, 110, 70},  // clay
    {20, 20, 20},    // coal
    {255, 255, 255}, // salt
    {175, 50, 30},   // iron
    {40, 175, 150},  // copper
    {120, 160, 210}, // tin
    {255, 205, 0},   // gold
}};

// Rarest first.
constexpr std::array rarity{
    planet::Resource::Gold, planet::Resource::Tin,  planet::Resource::Copper,
    planet::Resource::Salt, planet::Resource::Coal, planet::Resource::Iron,
    planet::Resource::Clay,
};

[[nodiscard]] double radians(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

[[nodiscard]] DVec3 direction_at(double latitude, double longitude) {
    return {std::cos(latitude) * std::cos(longitude), std::cos(latitude) * std::sin(longitude),
            std::sin(latitude)};
}

} // namespace

Rgb Image::at(int x, int y) const {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                             static_cast<std::size_t>(x));
    return {pixels.at(offset), pixels.at(offset + 1), pixels.at(offset + 2)};
}

ImageSize image_size(const View& view, int width) noexcept {
    if (std::holds_alternative<PlanetView>(view)) {
        return {.width = width, .height = std::max(width / 2, 1)};
    }
    return {.width = width, .height = width};
}

DVec3 pixel_direction(const View& view, ImageSize size, int x, int y, double planet_radius) {
    // Fractions across and down the image, at the pixel's centre.
    const double across = (x + 0.5) / size.width;
    const double down = (y + 0.5) / size.height;
    if (std::holds_alternative<PlanetView>(view)) {
        return direction_at((0.5 - down) * std::numbers::pi,
                            (across - 0.5) * 2.0 * std::numbers::pi);
    }
    if (const auto* face = std::get_if<FaceView>(&view)) {
        return glm::normalize(planet::face_uv_to_xyz(face->face, planet::st_to_uv(across),
                                                     planet::st_to_uv(1.0 - down)));
    }
    const auto& region = std::get<RegionView>(view);
    const DVec3 center = direction_at(radians(region.latitude), radians(region.longitude));
    const planet::TangentFrame frame = planet::tangent_frame(center);
    const double east = (across - 0.5) * region.width;
    const double north = (0.5 - down) * region.width * size.height / size.width;
    return glm::normalize(center + (((frame.east * east) + (frame.north * north)) / planet_radius));
}

Rgb height_colour(std::int64_t elevation) noexcept {
    const double metres = planet::fixed_to_double(elevation);
    return metres <= 0.0 ? interpolate(ocean_stops, metres) : interpolate(land_stops, metres);
}

Rgb biome_colour(planet::Biome biome) noexcept {
    const auto index = static_cast<std::size_t>(biome);
    return index < biome_colours.size() ? biome_colours.at(index) : Rgb{255, 0, 255};
}

Rgb temperature_colour(std::int64_t temperature) noexcept {
    return interpolate(temperature_stops, planet::fixed_to_double(temperature));
}

Rgb moisture_colour(std::int64_t moisture) noexcept {
    return interpolate(moisture_stops, planet::fixed_to_double(moisture));
}

Rgb deposit_colour(const std::array<planet::Deposit, planet::resource_count>& deposits,
                   std::int64_t elevation) noexcept {
    for (const planet::Resource resource : rarity) {
        if (!deposits.at(static_cast<std::size_t>(resource)).empty()) {
            return resource_colours.at(static_cast<std::size_t>(resource));
        }
    }
    if (elevation <= 0) {
        return {25, 35, 55};
    }
    const double height = std::clamp(planet::fixed_to_double(elevation) / 4'000.0, 0.0, 1.0);
    const auto grey = static_cast<std::uint8_t>(std::lround(90.0 + (120.0 * height)));
    return {grey, grey, grey};
}

Image render(const planet::Geology& geology, double planet_radius, const View& view, Layer layer,
             int width, jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const auto colour_at = [&](planet::CellId cell) -> Rgb {
        switch (layer) {
        case Layer::Height:
            return height_colour(geology.terrain().elevation(cell));
        case Layer::Biome:
            return biome_colour(geology.climate().biome(cell));
        case Layer::Temperature:
            return temperature_colour(geology.climate().temperature(cell));
        case Layer::Moisture:
            return moisture_colour(geology.climate().moisture(cell));
        case Layer::Deposits:
            return deposit_colour(geology.deposits(cell), geology.terrain().elevation(cell));
        }
        return {};
    };
    jobs.parallel_for(
        static_cast<std::uint32_t>(size.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                for (int x = 0; x < size.width; ++x) {
                    const Rgb colour = colour_at(
                        planet::cell_at(pixel_direction(view, size, x, y, planet_radius)));
                    const auto offset =
                        3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width)) +
                             static_cast<std::size_t>(x));
                    std::ranges::copy(colour,
                                      image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
                }
            }
        });
    return image;
}

std::string file_name(const View& view, Layer layer) {
    if (const auto* face = std::get_if<FaceView>(&view)) {
        return std::format("face{}-{}.png", face->face, to_string(layer));
    }
    return std::format("{}-{}.png", std::holds_alternative<PlanetView>(view) ? "planet" : "region",
                       to_string(layer));
}

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

} // namespace hog::mapgen
