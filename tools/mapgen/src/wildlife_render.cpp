#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

namespace hog::mapgen {

namespace {

constexpr Rgb shade{30, 30, 34};
constexpr Rgb sparse{190, 40, 40};
constexpr Rgb dense{255, 235, 90};

void put(Image& image, int x, int y, Rgb colour) {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
                             static_cast<std::size_t>(x));
    std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace

Rgb density_colour(Rgb ground, double share) noexcept {
    const Rgb dimmed = mix(ground, shade, 0.6);
    if (!(share > 0.0)) {
        return dimmed;
    }
    // A square root, so thin populations still show.
    const double shown = std::sqrt(std::min(share, 1.0));
    return mix(dimmed, mix(sparse, dense, shown), 0.35 + (0.65 * shown));
}

Image render_wildlife(const planet::Geology& geology, const sim::Wildlife& wildlife,
                      std::uint32_t species, double planet_radius, const View& view, int width,
                      jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const sim::CellGrid& grid = wildlife.grid();
    const std::vector<float>& count = wildlife.fields().populations[species].count;
    const double crowded = wildlife.config().ecology.species[species].crowded;
    jobs.parallel_for(
        static_cast<std::uint32_t>(size.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                for (int x = 0; x < size.width; ++x) {
                    const DVec3 direction = pixel_direction(view, size, x, y, planet_radius);
                    const double density = grid.interpolate(direction, [&](std::uint32_t cell) {
                        return static_cast<double>(count[cell]) / wildlife.area(cell);
                    });
                    const Rgb ground =
                        height_colour(geology.terrain().elevation(planet::cell_at(direction)));
                    put(image, x, y, density_colour(ground, density / crowded));
                }
            }
        });
    return image;
}

std::string wildlife_file_name(const View& view, std::string_view species, GameTime time) {
    // Same prefix as the layer maps, so a view's files sort together.
    const std::string layer_name = file_name(view, Layer::Height);
    const std::string prefix = layer_name.substr(0, layer_name.rfind('-'));
    std::string name{species};
    std::ranges::replace(name, ' ', '-');
    const auto minutes = std::chrono::floor<std::chrono::minutes>(
        std::chrono::sys_time<GameDuration>{time.time_since_epoch()});
    return std::format("{}-wildlife-{:%Y%m%dT%H%M}-{}.png", prefix, minutes, name);
}

std::string wildlife_header(const sim::Ecology& ecology) {
    std::string header = "time";
    for (const sim::Species& species : ecology.species) {
        header += std::format(",{}", species.name);
    }
    return header + ",forage,carrion";
}

std::string wildlife_row(const sim::Wildlife& wildlife) {
    const auto minutes = std::chrono::floor<std::chrono::minutes>(
        std::chrono::sys_time<GameDuration>{wildlife.time().time_since_epoch()});
    std::string row = std::format("{:%Y-%m-%dT%H:%M}", minutes);
    for (std::uint32_t species = 0; species < wildlife.fields().populations.size(); ++species) {
        row += std::format(",{:.0f}", wildlife.total(species));
    }
    double area = 0.0;
    double forage = 0.0;
    double carrion = 0.0;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        if (!wildlife.habitat()[cell].ocean) {
            area += wildlife.area(cell);
            forage += static_cast<double>(wildlife.fields().forage[cell]) * wildlife.area(cell);
            carrion += static_cast<double>(wildlife.fields().carrion[cell]) * wildlife.area(cell);
        }
    }
    return row + std::format(",{:.0f},{:.2f}", area > 0.0 ? forage / area : 0.0,
                             area > 0.0 ? carrion / area : 0.0);
}

} // namespace hog::mapgen
