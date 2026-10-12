#include <hog/app/probe_layers.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hog::app {

namespace {

void copy(std::span<const float> from, std::vector<float>& to) {
    to.assign(from.begin(), from.end());
}

void component(std::span<const Vec2> from, Vec2::length_type axis, std::vector<float>& to) {
    to.resize(from.size());
    std::ranges::transform(from, to.begin(), [axis](const Vec2& value) { return value[axis]; });
}

// Calls use(name, unit, fill) for every layer, where fill(values) writes one value per cell.
template <typename Use>
void each_layer(const LayerSources& sources, Use use) {
    const sim::CellGrid& grid = sources.weather.grid();
    const std::uint32_t size = grid.size();
    const double radius = sources.weather.config().planet_radius;
    const std::vector<sim::Ground>& ground = sources.weather.ground();
    const auto per_cell = [size](std::vector<float>& values, auto value) {
        values.resize(size);
        for (std::uint32_t cell = 0; cell < size; ++cell) {
            values[cell] = static_cast<float>(value(cell));
        }
    };

    use("planet/area", "km²", [&](std::vector<float>& values) {
        per_cell(values,
                 [&](std::uint32_t cell) { return grid.area(cell) * radius * radius / 1e6; });
    });
    use("planet/elevation", "m", [&](std::vector<float>& values) {
        per_cell(values, [&](std::uint32_t cell) { return ground[cell].elevation; });
    });
    use("planet/ocean", "", [&](std::vector<float>& values) {
        per_cell(values, [&](std::uint32_t cell) { return ground[cell].ocean ? 1.0F : 0.0F; });
    });
    use("planet/biome", "biome", [&](std::vector<float>& values) {
        per_cell(values, [&](std::uint32_t cell) {
            return static_cast<float>(sources.planet.state(grid.cell(cell)).biome);
        });
    });

    const sim::WeatherFields& weather = sources.weather.fields();
    use("weather/temperature", "°C", [&](auto& values) { copy(weather.temperature, values); });
    use("weather/upper_temperature", "°C",
        [&](auto& values) { copy(weather.upper_temperature, values); });
    use("weather/pressure", "hPa", [&](auto& values) { copy(weather.pressure, values); });
    use("weather/vapour", "mm", [&](auto& values) { copy(weather.vapour, values); });
    use("weather/cloud", "mm", [&](auto& values) { copy(weather.cloud, values); });
    use("weather/wind_east", "m/s", [&](auto& values) { component(weather.wind, 0, values); });
    use("weather/wind_north", "m/s", [&](auto& values) { component(weather.wind, 1, values); });
    use("weather/jet_east", "m/s", [&](auto& values) { component(weather.jet, 0, values); });
    use("weather/jet_north", "m/s", [&](auto& values) { component(weather.jet, 1, values); });
    // What the last step did, as hourly rates, since tools don't know the step.
    const double per_hour =
        3'600.0 / std::chrono::duration<double>(sources.weather.config().step).count();
    const auto hourly = [&](const std::vector<float>& from, std::vector<float>& values) {
        values.resize(from.size());
        std::ranges::transform(from, values.begin(), [per_hour](float amount) {
            return static_cast<float>(static_cast<double>(amount) * per_hour);
        });
    };
    use("weather/evaporation", "mm/h", [&](auto& values) { hourly(weather.evaporation, values); });
    use("weather/rain", "mm/h", [&](auto& values) { hourly(weather.rain, values); });
    use("weather/snow", "mm/h", [&](auto& values) { hourly(weather.snow, values); });
    use("weather/hail", "mm/h", [&](auto& values) { hourly(weather.hail, values); });
    use("weather/storm", "", [&](auto& values) { copy(weather.storm, values); });
    use("weather/lift", "m/s", [&](auto& values) { copy(weather.lift, values); });
    use("weather/front", "", [&](auto& values) { copy(weather.front, values); });
    use("weather/recent_temperature", "°C",
        [&](auto& values) { copy(weather.recent_temperature, values); });
    use("weather/recent_precipitation", "mm/day",
        [&](auto& values) { copy(weather.recent_precipitation, values); });

    if (const sim::Wildlife* wildlife = sources.wildlife) {
        const sim::WildlifeFields& fields = wildlife->fields();
        use("wildlife/forage", "kg/km²", [&](auto& values) { copy(fields.forage, values); });
        use("wildlife/carrion", "kg/km²", [&](auto& values) { copy(fields.carrion, values); });
        const auto& species = wildlife->config().ecology.species;
        for (std::size_t index = 0; index < species.size(); ++index) {
            const sim::Population& population = fields.populations[index];
            const std::string name = "wildlife/" + species[index].name;
            use(name, "animals", [&](auto& values) { copy(population.count, values); });
            use(name + "/fed", "", [&](auto& values) { copy(population.fed, values); });
        }
    }

    if (const sim::Forest* forest = sources.forest) {
        const sim::ForestFields& fields = forest->fields();
        use("forest/biomass", "kg/m²", [&](auto& values) { copy(fields.biomass, values); });
        use("forest/capacity", "kg/m²", [&](std::vector<float>& values) {
            per_cell(values, [&](std::uint32_t cell) { return forest->capacity(cell); });
        });
        use("forest/dryness", "", [&](auto& values) { copy(fields.dryness, values); });
        use("forest/fire", "", [&](auto& values) { copy(fields.fire, values); });
    }
}

} // namespace

std::vector<probe::LayerInfo> layer_catalogue(const LayerSources& sources) {
    std::vector<probe::LayerInfo> layers;
    each_layer(sources, [&layers](std::string_view name, std::string_view unit, auto&& /*fill*/) {
        layers.push_back(probe::LayerInfo{.name = std::string{name}, .unit = std::string{unit}});
    });
    return layers;
}

probe::Layers fetch_layers(const LayerSources& sources, const probe::Fetch& fetch, GameTime time) {
    probe::Layers layers{.time = time, .layers = {}};
    each_layer(sources, [&](std::string_view name, std::string_view /*unit*/, auto&& fill) {
        if (std::ranges::find(fetch.names, name) != fetch.names.end()) {
            probe::Layer& layer =
                layers.layers.emplace_back(probe::Layer{.name = std::string{name}, .values = {}});
            fill(layer.values);
        }
    });
    return layers;
}

} // namespace hog::app
