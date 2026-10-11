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
    double metres;
    Rgb colour;
};

constexpr std::array ocean_stops{
    Stop{.metres = -4'000.0, .colour = {8, 30, 80}},
    Stop{.metres = -1'000.0, .colour = {25, 70, 140}},
    Stop{.metres = -100.0, .colour = {60, 130, 195}},
    Stop{.metres = 0.0, .colour = {130, 190, 225}},
};

constexpr std::array land_stops{
    Stop{.metres = 0.0, .colour = {70, 140, 70}},
    Stop{.metres = 300.0, .colour = {150, 180, 95}},
    Stop{.metres = 1'000.0, .colour = {195, 165, 110}},
    Stop{.metres = 2'500.0, .colour = {125, 95, 75}},
    Stop{.metres = 4'000.0, .colour = {245, 245, 245}},
};

template <std::size_t N>
[[nodiscard]] Rgb interpolate(const std::array<Stop, N>& stops, double metres) {
    if (metres <= stops.front().metres) {
        return stops.front().colour;
    }
    for (std::size_t index = 1; index < N; ++index) {
        const Stop& high = stops.at(index);
        if (metres <= high.metres) {
            const Stop& low = stops.at(index - 1);
            const double t = (metres - low.metres) / (high.metres - low.metres);
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

Image render(const planet::Geology& geology, double planet_radius, const View& view, Layer layer,
             int width, jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const auto colour_at = [&](planet::CellId cell) -> Rgb {
        if (layer == Layer::Height) {
            return height_colour(geology.terrain().elevation(cell));
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
