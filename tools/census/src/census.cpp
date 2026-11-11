#include <hog/census/census.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace hog::census {

namespace {

constexpr std::array<std::string_view, globe::material_count> material_names{
    "air",       "water",         "snow",       "soil",    "sand",   "sediment",
    "sandstone", "shale",         "limestone",  "granite", "basalt", "mantle",
    "iron",      "anorthosite",   "regolith",   "firn",    "ice",    "clay",
    "gravel",    "volcanic sand", "shell sand", "shingle"};

constexpr double degree = std::numbers::pi / 180.0;

// Cells along a side of a square of columns read at once.
constexpr int square = 64;

// The level water stands at over `spot` off the sea, metres over the radius; none where it's dry
// or the sea.
std::optional<double> inland(const Spot& spot, std::optional<double> sea) {
    const double water = spot.water.value_or(-std::numeric_limits<double>::infinity());
    if (water <= spot.ground || (sea.has_value() && std::abs(water - sea.value_or(0.0)) <= still)) {
        return std::nullopt;
    }
    return water;
}

double km2(globe::Cell cell, double radius) {
    return globe::solid_angle(cell) * radius * radius / 1e6;
}

// The inland water joined to the spot at `first` cell to cell across their sides, whatever level
// each stands at, marking each `taken`.
Body join(int level, std::span<const Spot> spots, double radius, std::optional<double> sea,
          std::size_t first, std::vector<bool>& taken) {
    Body body{.level = std::numeric_limits<double>::infinity()};
    double highest = -std::numeric_limits<double>::infinity();
    std::size_t deepest = first;
    taken[first] = true;
    std::vector<std::size_t> waiting{first};
    while (!waiting.empty()) {
        const std::size_t index = waiting.back();
        waiting.pop_back();
        const double water = inland(spots[index], sea).value_or(0.0);
        const globe::Cell cell = globe::cell_at_index(level, index);
        body.area += km2(cell, radius);
        body.level = std::min(body.level, water);
        highest = std::max(highest, water);
        if (water - spots[index].ground > body.deepest) {
            body.deepest = water - spots[index].ground;
            deepest = index;
        }
        const globe::CellNeighbors beside = globe::sides(cell);
        for (const globe::Cell next : beside.all()) {
            const auto at = static_cast<std::size_t>(globe::index_of(next));
            if (!taken[at] && inland(spots[at], sea)) {
                taken[at] = true;
                waiting.push_back(at);
            }
        }
    }
    body.fall = highest - body.level;
    body.level -= sea.value_or(0.0);
    const DVec3 there = globe::center(globe::cell_at_index(level, deepest));
    body.latitude = std::asin(std::clamp(there.z, -1.0, 1.0)) / degree;
    body.longitude = std::atan2(there.y, there.x) / degree;
    return body;
}

} // namespace

Census count(int level, std::span<const Spot> spots, double radius, std::optional<double> sea) {
    Census census;
    census.level = level;
    census.cell = radius * (std::numbers::pi / 2.0) / static_cast<double>(1U << level);
    const double base = sea.value_or(0.0);
    census.lowest = std::numeric_limits<double>::infinity();
    census.highest = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < spots.size(); ++index) {
        const Spot& spot = spots[index];
        const double area = km2(globe::cell_at_index(level, index), radius);
        census.area += area;
        census.lowest = std::min(census.lowest, spot.ground - base);
        census.highest = std::max(census.highest, spot.ground - base);
        if (spot.water.value_or(spot.ground) <= spot.ground) {
            census.land += area;
            census.tops.at(static_cast<std::size_t>(spot.top)) += area;
        } else if (!inland(spot, sea)) {
            census.sea += area;
        }
    }
    std::vector<bool> taken(spots.size(), false);
    for (std::size_t first = 0; first < spots.size(); ++first) {
        if (!taken[first] && inland(spots[first], sea)) {
            const Body body = join(level, spots, radius, sea, first, taken);
            (body.fall > still ? census.running : census.lakes).push_back(body);
        }
    }
    const auto larger = [](const Body& a, const Body& b) { return a.area > b.area; };
    std::ranges::sort(census.lakes, larger);
    std::ranges::sort(census.running, larger);
    return census;
}

std::vector<Spot> survey(const globe::Generator& generator, int level, const globe::EachOf& each) {
    const double radius = generator.planet().body.radius;
    const double lod = radius * (std::numbers::pi / 2.0) / static_cast<double>(1U << level);
    const auto side = static_cast<std::uint32_t>(1U << level);
    const std::uint32_t across = std::max(1U, side / square);
    const int width = static_cast<int>(std::min<std::uint32_t>(side, square));
    const std::size_t per_face = std::size_t{across} * across;
    std::vector<Spot> spots(globe::cells_at(level));
    const auto read = [&](std::size_t k) {
        const auto block = static_cast<std::uint32_t>(k % per_face);
        const globe::Cell corner{.level = static_cast<std::uint8_t>(level),
                                 .face = static_cast<std::uint8_t>(k / per_face),
                                 .i = (block % across) * static_cast<std::uint32_t>(width),
                                 .j = (block / across) * static_cast<std::uint32_t>(width)};
        const std::vector<globe::Generator::Column> columns = generator.columns(corner, width, lod);
        for (int j = 0; j < width; ++j) {
            for (int i = 0; i < width; ++i) {
                const globe::Generator::Column& column =
                    columns[(static_cast<std::size_t>(j) * static_cast<std::size_t>(width)) +
                            static_cast<std::size_t>(i)];
                const globe::Cell cell{.level = corner.level,
                                       .face = corner.face,
                                       .i = corner.i + static_cast<std::uint32_t>(i),
                                       .j = corner.j + static_cast<std::uint32_t>(j)};
                Spot& spot = spots[static_cast<std::size_t>(globe::index_of(cell))];
                spot.ground = column.ground() - radius;
                if (column.water() > 0.0) {
                    spot.water = column.water() - radius;
                }
                // The voxel just under the ground, as the finest show it.
                spot.top = column.sample(column.ground() - (generator.planet().voxel / 2.0), false)
                               .material;
            }
        }
    };
    const std::size_t blocks = 6 * per_face;
    if (each) {
        each(blocks, read);
    } else {
        for (std::size_t k = 0; k < blocks; ++k) {
            read(k);
        }
    }
    return spots;
}

std::string report(const Census& census, std::size_t lakes) {
    const auto share = [&](double area) { return 100.0 * area / census.area; };
    std::string out = std::format("level {} ({:.0f} m cells), {:.0f} km²\n"
                                  "land {:.1f}%, sea {:.1f}%, ground {:.0f} to {:.0f} m\n",
                                  census.level, census.cell, census.area, share(census.land),
                                  share(census.sea), census.lowest, census.highest);
    const auto list = [&](std::string_view name, const std::vector<Body>& bodies) {
        double area = 0.0;
        for (const Body& body : bodies) {
            area += body.area;
        }
        out += std::format("{} water: {} bodies, {:.1f} km²\n", name, bodies.size(), area);
        for (std::size_t k = 0; k < std::min(lakes, bodies.size()); ++k) {
            const Body& body = bodies[k];
            out += std::format("  {:8.2f} km² at {:.2f},{:.2f}  level {:.1f} m  falls {:.1f} m  "
                               "deepest {:.0f} m\n",
                               body.area, body.latitude, body.longitude, body.level, body.fall,
                               body.deepest);
        }
    };
    list("still", census.lakes);
    list("running", census.running);
    std::vector<std::pair<double, std::size_t>> tops;
    for (std::size_t k = 0; k < census.tops.size(); ++k) {
        if (census.tops.at(k) > 0.0) {
            tops.emplace_back(census.tops.at(k), k);
        }
    }
    std::ranges::sort(tops, std::greater{});
    out += "land tops:";
    for (const auto& [area, material] : tops) {
        out += std::format(" {} {:.1f}%", material_names.at(material), 100.0 * area / census.land);
    }
    out += '\n';
    return out;
}

} // namespace hog::census
