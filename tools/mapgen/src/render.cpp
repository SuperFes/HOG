#include <hog/core/assert.hpp>
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

constexpr std::array ocean_stops{
    ColourStop{.at = -4'000.0, .colour = {8, 30, 80}},
    ColourStop{.at = -1'000.0, .colour = {25, 70, 140}},
    ColourStop{.at = -100.0, .colour = {60, 130, 195}},
    ColourStop{.at = 0.0, .colour = {130, 190, 225}},
};

constexpr std::array land_stops{
    ColourStop{.at = 0.0, .colour = {70, 140, 70}},
    ColourStop{.at = 300.0, .colour = {150, 180, 95}},
    ColourStop{.at = 1'000.0, .colour = {195, 165, 110}},
    ColourStop{.at = 2'500.0, .colour = {125, 95, 75}},
    ColourStop{.at = 4'000.0, .colour = {245, 245, 245}},
};

constexpr std::array temperature_stops{
    ColourStop{.at = -30.0, .colour = {60, 20, 120}},
    ColourStop{.at = -10.0, .colour = {60, 110, 220}},
    ColourStop{.at = 0.0, .colour = {245, 245, 245}},
    ColourStop{.at = 15.0, .colour = {245, 200, 80}},
    ColourStop{.at = 35.0, .colour = {190, 30, 30}},
};

// Rock temperature in °C, glowing as it nears magma.
constexpr std::array heat_stops{
    ColourStop{.at = 0.0, .colour = {30, 30, 60}},
    ColourStop{.at = 100.0, .colour = {110, 30, 90}},
    ColourStop{.at = 300.0, .colour = {200, 40, 30}},
    ColourStop{.at = 700.0, .colour = {250, 150, 20}},
    ColourStop{.at = 1'100.0, .colour = {255, 245, 180}},
};

constexpr std::array moisture_stops{
    ColourStop{.at = 0.0, .colour = {150, 100, 50}},
    ColourStop{.at = 0.3, .colour = {225, 205, 120}},
    ColourStop{.at = 0.6, .colour = {80, 170, 80}},
    ColourStop{.at = 1.0, .colour = {30, 80, 190}},
};

// Metres down to the water table.
constexpr std::array water_table_stops{
    ColourStop{.at = 0.0, .colour = {50, 150, 150}},
    ColourStop{.at = 2.0, .colour = {100, 170, 90}},
    ColourStop{.at = 10.0, .colour = {190, 185, 110}},
    ColourStop{.at = 50.0, .colour = {160, 115, 70}},
    ColourStop{.at = 200.0, .colour = {100, 70, 50}},
};

// River width in metres.
constexpr std::array river_stops{
    ColourStop{.at = 0.0, .colour = {90, 160, 230}},
    ColourStop{.at = 50.0, .colour = {20, 70, 200}},
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
    {80, 120, 100},  // wetland
    {60, 110, 190},  // lake
}};

// Indexed by cave: dark, each tinted after the rock it's found in.
constexpr std::array<Rgb, planet::cave_count> cave_colours{{
    {25, 25, 40}, // karst
    {15, 45, 35}, // sinkhole
    {50, 15, 10}, // lava tube
    {10, 30, 60}, // sea cave
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

// Indexed by rock.
constexpr std::array<Rgb, planet::rock_count> rock_colours{{
    {110, 80, 50},   // soil
    {225, 205, 150}, // sand
    {200, 140, 90},  // sandstone
    {95, 100, 110},  // shale
    {215, 210, 185}, // limestone
    {190, 150, 145}, // granite
    {55, 55, 60},    // basalt
    {95, 120, 70},   // peridotite
    {240, 90, 20},   // magma
}};

[[nodiscard]] double radians(double angle) {
    return angle * std::numbers::pi / 180.0;
}

[[nodiscard]] double degrees(double angle) {
    return angle * 180.0 / std::numbers::pi;
}

} // namespace

Rgb Image::at(int x, int y) const {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                             static_cast<std::size_t>(x));
    return {pixels.at(offset), pixels.at(offset + 1), pixels.at(offset + 2)};
}

View resolve(const View& view, const planet::Geology& geology) {
    const auto* islands = std::get_if<IslandsView>(&view);
    if (islands == nullptr) {
        return view;
    }
    const DVec3 center = geology.terrain().archipelago().center();
    return RegionView{.latitude = degrees(std::asin(center.z)),
                      .longitude = degrees(std::atan2(center.y, center.x)),
                      .width = islands->width};
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
        return planet::direction_at((0.5 - down) * std::numbers::pi,
                                    (across - 0.5) * 2.0 * std::numbers::pi);
    }
    if (const auto* face = std::get_if<FaceView>(&view)) {
        return glm::normalize(planet::face_uv_to_xyz(face->face, planet::st_to_uv(across),
                                                     planet::st_to_uv(1.0 - down)));
    }
    HOG_ASSERT(std::holds_alternative<RegionView>(view), "view not resolved");
    const auto& region = std::get<RegionView>(view);
    const DVec3 center = planet::direction_at(radians(region.latitude), radians(region.longitude));
    const planet::TangentFrame frame = planet::tangent_frame(center);
    const double east = (across - 0.5) * region.width;
    const double north = (0.5 - down) * region.width * size.height / size.width;
    return glm::normalize(center + (((frame.east * east) + (frame.north * north)) / planet_radius));
}

Rgb height_colour(std::int64_t elevation) noexcept {
    const double metres = planet::fixed_to_double(elevation);
    return metres <= 0.0 ? ramp(ocean_stops, metres) : ramp(land_stops, metres);
}

Rgb biome_colour(planet::Biome biome) noexcept {
    const auto index = static_cast<std::size_t>(biome);
    return index < biome_colours.size() ? biome_colours.at(index) : Rgb{255, 0, 255};
}

Rgb temperature_colour(std::int64_t temperature) noexcept {
    return ramp(temperature_stops, planet::fixed_to_double(temperature));
}

Rgb heat_colour(std::int64_t temperature) noexcept {
    return ramp(heat_stops, planet::fixed_to_double(temperature));
}

Rgb moisture_colour(std::int64_t moisture) noexcept {
    return ramp(moisture_stops, planet::fixed_to_double(moisture));
}

Rgb water_table_colour(std::int64_t water_table) noexcept {
    return ramp(water_table_stops, planet::fixed_to_double(water_table));
}

Rgb river_colour(double width) noexcept {
    return ramp(river_stops, width);
}

Rgb with_water(Rgb ground, bool lake, const planet::Reach* river) noexcept {
    if (lake) {
        return biome_colour(planet::Biome::Lake);
    }
    if (river != nullptr) {
        return river_colour(river->width);
    }
    return ground;
}

Rgb rock_colour(planet::Rock rock) noexcept {
    return rock_colours.at(static_cast<std::size_t>(rock));
}

Rgb cave_colour(planet::Cave cave) noexcept {
    return cave_colours.at(static_cast<std::size_t>(cave));
}

Rgb resource_colour(planet::Resource resource) noexcept {
    return resource_colours.at(static_cast<std::size_t>(resource));
}

Rgb deposit_colour(const std::array<planet::Deposit, planet::resource_count>& deposits,
                   std::int64_t elevation) noexcept {
    for (const planet::Resource resource : planet::rarest_first) {
        if (!deposits.at(static_cast<std::size_t>(resource)).empty()) {
            return resource_colour(resource);
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
    const View resolved = resolve(view, geology);
    const ImageSize size = image_size(resolved, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    // Half a pixel, in metres, at a pixel's centre.
    const auto margin = [&](DVec3 at, int x, int y) {
        const int other = y + 1 < size.height ? y + 1 : y - 1;
        return 0.5 * glm::length(at - pixel_direction(resolved, size, x, other, planet_radius)) *
               planet_radius;
    };
    const planet::Elevation& terrain = geology.terrain();
    // Rivers are drawn at least a pixel wide, so they show at any scale.
    const auto over_land = [&](Rgb ground, planet::CellId cell, DVec3 at, int x, int y) {
        if (terrain.elevation(cell) <= 0) {
            return ground;
        }
        return with_water(ground, terrain.lake(cell).has_value(),
                          terrain.rivers().near(cell, margin(at, x, y)));
    };
    const auto colour_at = [&](planet::CellId cell, DVec3 at, int x, int y) -> Rgb {
        switch (layer) {
        case Layer::Height:
            return over_land(height_colour(terrain.elevation(cell)), cell, at, x, y);
        case Layer::Biome:
            return over_land(biome_colour(geology.climate().biome(cell)), cell, at, x, y);
        case Layer::Temperature:
            return temperature_colour(geology.climate().temperature(cell));
        case Layer::Moisture:
            return moisture_colour(geology.climate().moisture(cell));
        case Layer::Deposits:
            return deposit_colour(geology.deposits(cell), terrain.elevation(cell));
        case Layer::Water: {
            const std::int64_t elevation = terrain.elevation(cell);
            const Rgb ground = elevation <= 0
                                   ? height_colour(elevation)
                                   : water_table_colour(geology.climate().water_table(cell));
            return over_land(ground, cell, at, x, y);
        }
        }
        return {};
    };
    jobs.parallel_for(
        static_cast<std::uint32_t>(size.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                for (int x = 0; x < size.width; ++x) {
                    const DVec3 at = pixel_direction(resolved, size, x, y, planet_radius);
                    const Rgb colour = colour_at(planet::cell_at(at), at, x, y);
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
    const char* name = "region";
    if (std::holds_alternative<PlanetView>(view)) {
        name = "planet";
    } else if (std::holds_alternative<IslandsView>(view)) {
        name = "islands";
    }
    return std::format("{}-{}.png", name, to_string(layer));
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
