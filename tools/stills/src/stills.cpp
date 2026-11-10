#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>
#include <hog/globe/maths.hpp>
#include <hog/stills/stills.hpp>

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

namespace hog::stills {
namespace {

using Rgb = std::array<std::uint8_t, 3>;
using globe::Generator;
using globe::Material;

constexpr double degrees = std::numbers::pi / 180.0;

// Splits the rows among threads; each row is written by one only.
void each_row(int rows, int threads, const auto& body) {
    std::vector<std::thread> workers;
    const int count = std::clamp(threads, 1, std::max(rows, 1));
    workers.reserve(static_cast<std::size_t>(count));
    for (int worker = 0; worker < count; ++worker) {
        workers.emplace_back([&, worker] {
            for (int row = worker; row < rows; row += count) {
                body(row);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

Rgb mixed(Rgb from, Rgb to, double t) {
    const double s = std::clamp(t, 0.0, 1.0);
    Rgb out{};
    for (std::size_t k = 0; k < 3; ++k) {
        out.at(k) =
            static_cast<std::uint8_t>(std::lround(from.at(k) + ((to.at(k) - from.at(k)) * s)));
    }
    return out;
}

Rgb ramp(std::span<const std::pair<double, Rgb>> stops, double t) {
    if (t <= stops.front().first) {
        return stops.front().second;
    }
    for (std::size_t k = 1; k < stops.size(); ++k) {
        if (t <= stops[k].first) {
            const auto& [low, from] = stops[k - 1];
            const auto& [high, to] = stops[k];
            return mixed(from, to, (t - low) / (high - low));
        }
    }
    return stops.back().second;
}

Rgb shaded(Rgb colour, double light) {
    Rgb out{};
    for (std::size_t k = 0; k < 3; ++k) {
        out.at(k) =
            static_cast<std::uint8_t>(std::clamp(std::lround(colour.at(k) * light), 0L, 255L));
    }
    return out;
}

Rgb material_colour(Material material) {
    switch (material) {
    case Material::Air:
        return {205, 225, 250};
    case Material::Water:
        return {40, 90, 170};
    case Material::Snow:
        return {245, 245, 250};
    case Material::Soil:
        return {105, 78, 50};
    case Material::Sand:
        return {222, 200, 140};
    case Material::Sediment:
        return {160, 140, 110};
    case Material::Sandstone:
        return {200, 148, 98};
    case Material::Shale:
        return {98, 98, 110};
    case Material::Limestone:
        return {212, 206, 186};
    case Material::Granite:
        return {190, 158, 150};
    case Material::Basalt:
        return {62, 60, 64};
    case Material::Mantle:
        return {92, 120, 62};
    case Material::Iron:
        return {120, 120, 132};
    case Material::Anorthosite:
        return {200, 200, 194};
    case Material::Regolith:
        return {150, 146, 140};
    case Material::Firn:
        return {208, 220, 232};
    case Material::Ice:
        return {140, 190, 228};
    case Material::Clay:
        return {150, 102, 70};
    case Material::Gravel:
        return {120, 110, 98};
    case Material::VolcanicSand:
        return {34, 34, 36};
    case Material::ShellSand:
        return {230, 222, 200};
    case Material::Shingle:
        return {128, 122, 115};
    }
    return {255, 0, 255};
}

// Heights over the radius on a grid of directions, and how far apart its points are.
struct Heights {
    int width = 0;
    int height = 0;
    std::vector<double> values;

    [[nodiscard]] double at(int x, int y) const {
        return values[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                      static_cast<std::size_t>(x)];
    }
};

Heights heights(const Generator& generator, int width, int height, double lod, int threads,
                const auto& direction_at) {
    Heights grid{.width = width,
                 .height = height,
                 .values = std::vector<double>(static_cast<std::size_t>(width) *
                                               static_cast<std::size_t>(height))};
    const double radius = generator.planet().body.radius;
    const double floor = generator.layers().floor - radius;
    each_row(height, threads, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const auto ground = globe::ground_below(generator, direction_at(x, y), lod);
            grid.values[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                        static_cast<std::size_t>(x)] = ground ? *ground - radius : floor;
        }
    });
    return grid;
}

Rgb ground_colour(const Generator& generator, double height, double low, double high) {
    if (generator.sea_level() && height < 0.0) {
        return mixed({95, 165, 215}, {10, 30, 90}, -height / 6'000.0);
    }
    if (generator.sea_level()) {
        static constexpr std::array<std::pair<double, Rgb>, 5> land{{{0.0, {70, 120, 60}},
                                                                     {0.15, {140, 150, 80}},
                                                                     {0.35, {150, 118, 80}},
                                                                     {0.6, {125, 112, 102}},
                                                                     {0.85, {242, 242, 242}}}};
        return ramp(land, height / std::max(generator.shape().highest, 1.0));
    }
    static constexpr std::array<std::pair<double, Rgb>, 3> dry{
        {{0.0, {55, 48, 44}}, {0.5, {150, 132, 112}}, {1.0, {236, 228, 210}}}};
    return ramp(dry, (height - low) / std::max(high - low, 1.0));
}

// Lit from the north-west, high; `spacing` is metres between columns east and rows north.
Image shade(const Generator& generator, const Heights& grid, double east_spacing,
            const auto& east_spacing_at, double north_spacing, bool wraps) {
    const auto [low, high] = std::ranges::minmax(grid.values);
    const DVec3 light = glm::normalize(DVec3{-1.0, 1.0, 1.4});
    Image image{.width = grid.width,
                .height = grid.height,
                .pixels = std::vector<std::uint8_t>(3 * grid.values.size())};
    for (int y = 0; y < grid.height; ++y) {
        const double across = east_spacing * east_spacing_at(y);
        for (int x = 0; x < grid.width; ++x) {
            const int west = wraps ? (x + grid.width - 1) % grid.width : std::max(x - 1, 0);
            const int east = wraps ? (x + 1) % grid.width : std::min(x + 1, grid.width - 1);
            const int north = std::max(y - 1, 0);
            const int south = std::min(y + 1, grid.height - 1);
            const double slope_east =
                (grid.at(east, y) - grid.at(west, y)) / (across * (east - west == 0 ? 1 : 2));
            const double slope_north =
                (grid.at(x, north) - grid.at(x, south)) / (north_spacing * (south - north));
            const DVec3 normal = glm::normalize(DVec3{-slope_east, -slope_north, 1.0});
            const double height = grid.at(x, y);
            const bool sea = generator.sea_level() && height < 0.0;
            const double lit = sea ? 1.0 : 0.45 + (0.75 * std::max(0.0, glm::dot(normal, light)));
            const Rgb colour = shaded(ground_colour(generator, height, low, high), lit);
            const auto offset =
                3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.width)) +
                     static_cast<std::size_t>(x));
            std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    return image;
}

struct Frame {
    DVec3 up;
    DVec3 east;
    DVec3 north;
    DVec3 forward;
};

Frame frame_at(const Place& place) {
    const double lat = place.latitude * degrees;
    const double lon = place.longitude * degrees;
    const double heading = place.heading * degrees;
    const DVec3 north{-globe::maths::sin(lat) * globe::maths::cos(lon),
                      -globe::maths::sin(lat) * globe::maths::sin(lon), globe::maths::cos(lat)};
    const DVec3 east{-globe::maths::sin(lon), globe::maths::cos(lon), 0.0};
    return {.up = place.direction(),
            .east = east,
            .north = north,
            .forward = (north * globe::maths::cos(heading)) + (east * globe::maths::sin(heading))};
}

} // namespace

std::array<std::uint8_t, 3> Image::at(int x, int y) const {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                             static_cast<std::size_t>(x));
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

DVec3 Place::direction() const noexcept {
    const double lat = latitude * degrees;
    const double lon = longitude * degrees;
    return {globe::maths::cos(lat) * globe::maths::cos(lon),
            globe::maths::cos(lat) * globe::maths::sin(lon), globe::maths::sin(lat)};
}

Image map(const Generator& generator, int width, int threads) {
    const int height = std::max(width / 2, 1);
    const double radius = generator.planet().body.radius;
    const double spacing = 2.0 * std::numbers::pi * radius / width;
    const auto direction_at = [&](int x, int y) {
        return Place{.latitude = 90.0 - ((y + 0.5) * 180.0 / height),
                     .longitude = -180.0 + ((x + 0.5) * 360.0 / width)}
            .direction();
    };
    const Heights grid = heights(generator, width, height, spacing, threads, direction_at);
    const auto shrink = [&](int y) {
        return std::max(globe::maths::cos((90.0 - ((y + 0.5) * 180.0 / height)) * degrees), 0.05);
    };
    return shade(generator, grid, spacing, shrink, spacing, true);
}

namespace {

// How many cells hold a lake: above the sea, with water standing deeper than half a metre.
std::size_t lake_cells(const std::vector<float>& heights, int level, double radius, double sea) {
    const globe::Drainage found = globe::drain(heights, level, radius, sea);
    std::size_t held = 0;
    for (std::uint32_t cell = 0; cell < heights.size(); ++cell) {
        held += static_cast<double>(heights[cell]) > sea && found.depth(cell) > 0.5 ? 1 : 0;
    }
    return held;
}

// `ground` worn by `erosion`, saying on stderr what it did to the lakes and how deep it cut.
std::vector<float> worn_by(const std::vector<float>& ground, int level, double radius, double sea,
                           const globe::Erosion& erosion) {
    const std::size_t before = lake_cells(ground, level, radius, sea);
    std::vector<float> worn = globe::erode(ground, level, radius, sea, erosion);
    double most = 0.0;
    double total = 0.0;
    for (std::size_t cell = 0; cell < worn.size(); ++cell) {
        most = std::max(most, static_cast<double>(ground[cell] - worn[cell]));
        total += static_cast<double>(ground[cell] - worn[cell]);
    }
    std::cerr << std::format("lake cells {} -> {}, cut at most {:.1f} m, on average {:.3f} m\n",
                             before, lake_cells(worn, level, radius, sea), most,
                             total / static_cast<double>(worn.size()));
    return worn;
}

} // namespace

Image drainage(const Generator& generator, int width, int level, int threads,
               const globe::Erosion& erosion) {
    const double radius = generator.planet().body.radius;
    const std::uint64_t count = globe::cells_at(level);
    const double lod = radius * std::numbers::pi / 2.0 / static_cast<double>(1 << level);
    std::vector<float> ground(static_cast<std::size_t>(count));
    constexpr std::uint64_t block = 4'096;
    each_row(static_cast<int>((count + block - 1) / block), threads, [&](int row) {
        const std::uint64_t first = static_cast<std::uint64_t>(row) * block;
        for (std::uint64_t cell = first; cell < std::min(first + block, count); ++cell) {
            const DVec3 up = globe::center(globe::cell_at_index(level, cell));
            ground[static_cast<std::size_t>(cell)] =
                static_cast<float>(generator.column(up, lod).ground() - radius);
        }
    });
    const double sea = generator.sea_level() ? 0.0 : -1e30;
    if (erosion.steps > 0) {
        ground = worn_by(ground, level, radius, sea, erosion);
    }
    const globe::Drainage drained = globe::drain(ground, level, radius, sea);

    const int height = std::max(width / 2, 1);
    Image image{.width = width,
                .height = height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(height))};
    const double tallest = std::max(generator.shape().highest, 1.0);
    each_row(height, threads, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const DVec3 up = Place{.latitude = 90.0 - ((y + 0.5) * 180.0 / height),
                                   .longitude = -180.0 + ((x + 0.5) * 360.0 / width)}
                                 .direction();
            const auto cell =
                static_cast<std::size_t>(globe::index_of(globe::cell_under(up, level)));
            const auto over = static_cast<double>(drained.ground[cell]);
            Rgb colour{};
            if (over <= sea) {
                colour = mixed({95, 165, 215}, {10, 30, 90}, -over / (0.5 * tallest));
            } else if (drained.depth(static_cast<std::uint32_t>(cell)) > 0.5) {
                // A hollow that holds water: deeper is darker.
                colour = mixed({70, 120, 220}, {20, 40, 140},
                               drained.depth(static_cast<std::uint32_t>(cell)) / 30.0);
            } else {
                colour = mixed({96, 110, 80}, {200, 190, 170}, over / tallest);
                // Rivers by what drains through: from a stream, 0.1 km², to the largest.
                const double drains = drained.catchment[cell];
                if (drains > 1.0e5) {
                    const double strength = std::log10(drains / 1.0e5) / 4.0;
                    colour = mixed(colour, {40, 150, 255}, 0.4 + (0.6 * std::min(strength, 1.0)));
                }
            }
            const std::size_t at =
                3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(x));
            for (std::size_t k = 0; k < 3; ++k) {
                image.pixels[at + k] = colour.at(k);
            }
        }
    });
    return image;
}

Image relief(const Generator& generator, const Place& place, double span, int size, int threads) {
    const Frame frame = frame_at(place);
    const double radius = generator.planet().body.radius;
    const double spacing = span / size;
    const auto direction_at = [&](int x, int y) {
        const double u = ((x + 0.5) * spacing) - (span / 2.0);
        const double v = (span / 2.0) - ((y + 0.5) * spacing);
        return glm::normalize(frame.up + (((u * frame.east) + (v * frame.north)) / radius));
    };
    const Heights grid = heights(generator, size, size, spacing, threads, direction_at);
    return shade(generator, grid, spacing, [](int) { return 1.0; }, spacing, false);
}

Image section(const Generator& generator, const Place& place, double length, double bottom,
              double top, int width, int threads) {
    const Frame frame = frame_at(place);
    const double radius = generator.planet().body.radius;
    const double spacing = length / width;
    const int height = std::max(static_cast<int>(std::lround((top - bottom) / spacing)), 1);
    Image image{.width = width,
                .height = height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(height))};
    each_row(height, threads, [&](int y) {
        const double over = top - ((y + 0.5) * spacing);
        for (int x = 0; x < width; ++x) {
            const double angle = (((x + 0.5) * spacing) - (length / 2.0)) / radius;
            const DVec3 direction =
                (frame.up * globe::maths::cos(angle)) + (frame.forward * globe::maths::sin(angle));
            const globe::Sample sample = generator.sample(direction * (radius + over), spacing);
            Rgb colour = material_colour(sample.material);
            if (sample.distance >= 0.0 && sample.water > 0.0) {
                colour = material_colour(Material::Water);
            }
            const auto offset =
                3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(x));
            std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    });
    return image;
}

Image voxel_section(const globe::World& world, const Place& place, double length, double bottom,
                    double top, int width) {
    const Frame frame = frame_at(place);
    const globe::Layers& layers = world.layers();
    const double radius = world.planet().body.radius;
    const double spacing = length / width;
    const int height = std::max(static_cast<int>(std::lround((top - bottom) / spacing)), 1);
    Image image{.width = width,
                .height = height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(height))};
    // Bricks are filled whole, so each is filled once.
    std::map<globe::BrickSite, globe::BrickVoxels> bricks;
    for (int y = 0; y < height; ++y) {
        const int layer = layers.layer_at(radius + top - ((y + 0.5) * spacing));
        for (int x = 0; x < width; ++x) {
            const double angle = (((x + 0.5) * spacing) - (length / 2.0)) / radius;
            const DVec3 direction =
                (frame.up * globe::maths::cos(angle)) + (frame.forward * globe::maths::sin(angle));
            Rgb colour = material_colour(Material::Air);
            if (layer >= 0 && layer < layers.count) {
                const globe::Cell cell = globe::cell_under(direction, layers.level);
                const globe::Site site{.face = cell.face,
                                       .i = cell.i,
                                       .j = cell.j,
                                       .layer = static_cast<std::uint32_t>(layer)};
                const globe::BrickSite brick = globe::brick_of(site);
                auto found = bricks.find(brick);
                if (found == bricks.end()) {
                    found = bricks.emplace(brick, world.brick(brick)).first;
                }
                const globe::Voxel& voxel =
                    found->second[static_cast<std::size_t>(globe::index_in_brick(site))];
                if (voxel.solid != Material::Air) {
                    colour = material_colour(voxel.solid);
                } else if (voxel.liquid_amount > globe::Voxel::full / 2) {
                    colour = material_colour(voxel.liquid);
                }
            }
            const auto offset =
                3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(x));
            std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    return image;
}

Image plan(const Generator& generator, const Place& place, double span, double depth, int size,
           int threads) {
    const Frame frame = frame_at(place);
    const double radius = generator.planet().body.radius;
    const double spacing = span / size;
    Image image{.width = size,
                .height = size,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size) *
                                                    static_cast<std::size_t>(size))};
    each_row(size, threads, [&](int y) {
        for (int x = 0; x < size; ++x) {
            const double u = ((x + 0.5) * spacing) - (span / 2.0);
            const double v = (span / 2.0) - ((y + 0.5) * spacing);
            const DVec3 direction =
                glm::normalize(frame.up + (((u * frame.east) + (v * frame.north)) / radius));
            const double ground =
                globe::ground_below(generator, direction, spacing).value_or(radius);
            const globe::Sample sample =
                generator.column(direction, spacing).sample(ground - depth);
            Rgb colour = material_colour(sample.material);
            if (sample.distance >= 0.0) {
                colour = sample.water > 0.0 ? material_colour(Material::Water) : Rgb{20, 20, 24};
            }
            const auto offset =
                3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                     static_cast<std::size_t>(x));
            std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    });
    return image;
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

} // namespace hog::stills
