#include <hog/core/hash.hpp>
#include <hog/formation_lab/pictures.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace hog::formation_lab {

DVec3 map_direction(int x, int y, int width, int height) {
    const double longitude = ((((x + 0.5) / width) * 2.0) - 1.0) * std::numbers::pi;
    const double latitude = (0.5 - ((y + 0.5) / height)) * std::numbers::pi;
    return {std::cos(latitude) * std::cos(longitude), std::cos(latitude) * std::sin(longitude),
            std::sin(latitude)};
}

Image map_picture(int width, const std::function<Rgb(const DVec3&)>& colour) {
    Image image{.width = width, .height = width / 2, .pixels = {}};
    image.pixels.resize(static_cast<std::size_t>(image.width) *
                        static_cast<std::size_t>(image.height) * 3);
    std::size_t at = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const Rgb pixel = colour(map_direction(x, y, image.width, image.height));
            image.pixels[at++] = pixel.r;
            image.pixels[at++] = pixel.g;
            image.pixels[at++] = pixel.b;
        }
    }
    return image;
}

void draw_line(Image& image, const DVec3& from, const DVec3& to, Rgb colour, int width) {
    const double across = std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
    const double pixel = std::numbers::pi / image.height;
    const int steps = std::max(1, static_cast<int>(std::ceil(across / (0.5 * pixel))));
    const int reach = std::max(0, (width - 1) / 2);
    for (int step = 0; step <= steps; ++step) {
        const DVec3 at = glm::normalize(glm::mix(from, to, static_cast<double>(step) / steps));
        const double longitude = std::atan2(at.y, at.x);
        const double latitude = std::asin(std::clamp(at.z, -1.0, 1.0));
        const auto x = static_cast<int>(((longitude / std::numbers::pi) + 1.0) * 0.5 * image.width);
        const auto y = static_cast<int>((0.5 - (latitude / std::numbers::pi)) * image.height);
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const int px = (((x + dx) % image.width) + image.width) % image.width;
                const int py = std::clamp(y + dy, 0, image.height - 1);
                const std::size_t at_pixel =
                    ((static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width)) +
                     static_cast<std::size_t>(px)) *
                    3;
                image.pixels[at_pixel] = colour.r;
                image.pixels[at_pixel + 1] = colour.g;
                image.pixels[at_pixel + 2] = colour.b;
            }
        }
    }
}

Rgb ramp(double t, std::span<const Rgb> stops) {
    if (stops.size() == 1) {
        return stops.front();
    }
    const double along = std::clamp(t, 0.0, 1.0) * static_cast<double>(stops.size() - 1);
    const auto below = std::min(static_cast<std::size_t>(along), stops.size() - 2);
    const double share = along - static_cast<double>(below);
    const auto mix = [share](std::uint8_t a, std::uint8_t b) {
        return static_cast<int>(std::lround(a + ((b - a) * share)));
    };
    const Rgb& a = stops[below];
    const Rgb& b = stops[below + 1];
    return rgb(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

Rgb height_colour(double metres) {
    static constexpr std::array<Rgb, 4> sea{rgb(8, 20, 60), rgb(16, 50, 110), rgb(40, 100, 170),
                                            rgb(110, 170, 210)};
    static constexpr std::array<Rgb, 6> land{rgb(70, 120, 60),   rgb(120, 150, 80),
                                             rgb(170, 160, 100), rgb(140, 110, 80),
                                             rgb(170, 160, 150), rgb(245, 245, 245)};
    if (metres < 0.0) {
        return ramp(1.0 + (metres / 7'000.0), sea);
    }
    return ramp(metres / 6'000.0, land);
}

Rgb plate_colour(std::uint16_t plate) {
    const std::uint64_t bits = hash_values(0x51A7E5, plate);
    return rgb(static_cast<int>(80 + (bits & 0x7FU)), static_cast<int>(80 + ((bits >> 8U) & 0x7FU)),
               static_cast<int>(80 + ((bits >> 16U) & 0x7FU)));
}

Rgb meeting_colour(formation::Meeting meeting) {
    using formation::Meeting;
    switch (meeting) {
    case Meeting::Collision:
        return rgb(200, 30, 30);
    case Meeting::Subduction:
        return rgb(230, 120, 20);
    case Meeting::IslandArc:
        return rgb(230, 200, 30);
    case Meeting::Ridge:
        return rgb(30, 200, 230);
    case Meeting::Rift:
        return rgb(160, 60, 200);
    case Meeting::Transform:
        return rgb(240, 240, 240);
    case Meeting::None:
        break;
    }
    return rgb(0, 0, 0);
}

Rgb scaled(Rgb colour, double by) {
    const auto one = [by](std::uint8_t channel) {
        return static_cast<int>(std::clamp(std::lround(channel * by), 0L, 255L));
    };
    return rgb(one(colour.r), one(colour.g), one(colour.b));
}

double shade(const formation::Sphere& sphere, std::span<const double> heights,
             const DVec3& direction, double radius, double step, double exaggeration,
             std::uint32_t& start) {
    const DVec3 pole{0.0, 0.0, 1.0};
    const DVec3 east_axis = glm::cross(pole, direction);
    if (glm::length(east_axis) < 1e-9) {
        return 1.0;
    }
    const DVec3 east = glm::normalize(east_axis);
    const DVec3 north = glm::cross(direction, east);
    const auto height_at = [&](const DVec3& where) {
        const formation::Sphere::Spot spot = sphere.locate(glm::normalize(where), start);
        start = spot.corners[0];
        return sphere.sample(heights, spot);
    };
    const double across = 2.0 * step * radius;
    const double rise_east =
        (height_at(direction + (step * east)) - height_at(direction - (step * east))) / across;
    const double rise_north =
        (height_at(direction + (step * north)) - height_at(direction - (step * north))) / across;
    // Lit from the north-west, high in the sky.
    const DVec3 normal =
        glm::normalize(DVec3{-rise_east * exaggeration, -rise_north * exaggeration, 1.0});
    const DVec3 light = glm::normalize(DVec3{-1.0, 1.0, 1.4});
    const double flat = light.z;
    return std::clamp(glm::dot(normal, light) / flat, 0.25, 1.6);
}

} // namespace hog::formation_lab
