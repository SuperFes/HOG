#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/fields.hpp>
#include <hog/formation/ores.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hog::formation {

namespace {

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

double bell(double x) noexcept {
    return maths::exp(-(x * x));
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

// Share of heavy grains a river still carries on from one point to the next: gold and tin drop out
// within a few hundred kilometres of where they're worn loose.
constexpr double heavy_carried = 0.7;

// Heavy grains worn off `source` and carried down the rivers, highest ground first, dropped where
// the rivers lay down gravel.
std::vector<double> placed(const Rivers& rivers, const Sediment& sediment,
                           const std::vector<double>& source) {
    const auto size = static_cast<std::uint32_t>(rivers.receiver.size());
    std::vector<std::uint32_t> order;
    for (std::uint32_t point = 0; point < size; ++point) {
        if (!in_sea(rivers, point)) {
            order.push_back(point);
        }
    }
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return rivers.height[a] > rivers.height[b];
    });
    std::vector<double> arriving(size, 0.0);
    std::vector<double> placers(size, 0.0);
    for (const std::uint32_t point : order) {
        placers[point] = arriving[point] * smoothstep(0.0, 20.0, sediment.alluvium[point]);
        const double carried = std::max(source[point], arriving[point]) * heavy_carried;
        const std::uint32_t down = rivers.receiver[point];
        arriving[down] = std::max(arriving[down], carried);
    }
    return placers;
}

// What the ores at a point are made from.
struct Here {
    Rock rock = Rock::Basement;
    Meeting meeting = Meeting::None;
    double near = 0.0;   // belt widths to the nearest plate boundary
    double light = 0.0;  // share of light crust
    double eroded = 0.0; // metres worn off the top
    double cover = 0.0;  // metres of layered rock left
    double folding = 0.0;
    double soluble = 0.0;
    double celsius = 0.0;
    double wet = 0.0; // rain against what the air takes
    double wear = 0.0;
    double peat = 0.0; // metres of litter or peat
    bool long_weathered = false;
    bool salt_flat = false;
    // The planet's seeded history: where swamps, oil's source beds, salt basins, deep-rooted pipes
    // and gold-bearing veins happened to lie.
    double swamps = 0.0;
    double source_beds = 0.0;
    double salt_basins = 0.0;
    double pipes = 0.0;
    double veins = 0.0;

    [[nodiscard]] bool beds() const { return rock == Rock::Layered || rock == Rock::Folded; }
    [[nodiscard]] double flat() const { return 1.0 - smoothstep(0.2, 0.5, folding); }
    // Basement at the surface or under little cover.
    [[nodiscard]] double exposed_core() const {
        return rock == Rock::Basement ? 1.0 : 1.0 - smoothstep(0.0, 500.0, cover);
    }
};

// Copper forms a few kilometres under arc volcanoes: worn down to, not yet worn away.
double copper_at(const Here& here) {
    if (here.meeting != Meeting::Subduction && here.meeting != Meeting::IslandArc) {
        return 0.0;
    }
    const double over = here.meeting == Meeting::Subduction ? here.light : 1.0;
    return over * bell((here.near - 0.7) / 0.5) * smoothstep(300.0, 2'000.0, here.eroded) *
           (1.0 - smoothstep(6'000.0, 10'000.0, here.eroded));
}

// Gold veins thread parts of folded belts, best where worn into their deeper rock.
double gold_at(const Here& here) {
    const bool deep_rock = here.rock == Rock::Folded || here.rock == Rock::Basement;
    return here.folding * (deep_rock ? 1.0 : 0.3) * smoothstep(0.1, 0.5, here.veins);
}

// Ancient seas laid iron in beds on the old stable cores of continents, far from any boundary.
double banded_iron_at(const Here& here) {
    return here.light * smoothstep(3.0, 6.0, here.near) * here.exposed_core() *
           smoothstep(0.2, 0.5, here.source_beds);
}

// Only soil left a very long time, hot, wet and still enough to keep it, leaches to iron and
// aluminium.
double laterite_at(const Here& here) {
    if (!here.long_weathered) {
        return 0.0;
    }
    return smoothstep(18.0, 26.0, here.celsius) * smoothstep(0.8, 1.5, here.wet) *
           smoothstep(5.0e-5, 1.0e-5, here.wear);
}

double bog_iron_at(const Here& here) {
    return smoothstep(0.1, 1.0, here.peat) * waterlogged(here.wet);
}

// Brines from salt basins carry lead and zinc through limestone and lay them there.
double lead_zinc_at(const Here& here) {
    if (here.rock != Rock::Layered) {
        return 0.0;
    }
    return 2.0 * here.soluble * smoothstep(200.0, 2'000.0, here.cover) * here.flat() *
           smoothstep(0.1, 0.4, here.salt_basins);
}

// Salt lies where water dries out now, and in the soluble beds of basins that dried long ago.
double salt_at(const Here& here) {
    if (here.salt_flat) {
        return 1.0;
    }
    if (!here.beds()) {
        return 0.0;
    }
    return here.soluble * smoothstep(1.0, 0.3, here.wet) * smoothstep(0.3, 0.6, here.salt_basins);
}

// Old swamps' peat turns to coal buried under a kilometre or so of rock, and is cooked away deeper.
double coal_at(const Here& here) {
    if (!here.beds()) {
        return 0.0;
    }
    return smoothstep(300.0, 1'500.0, here.cover) *
           (1.0 - smoothstep(4'000.0, 8'000.0, here.cover)) * smoothstep(0.2, 0.5, here.swamps);
}

// Oil cooks out of deep beds and gathers under gentle folds.
double oil_at(const Here& here) {
    if (!here.beds()) {
        return 0.0;
    }
    const double trapped = 0.5 + (0.5 * bell((here.folding - 0.3) / 0.25));
    return smoothstep(2'000.0, 5'000.0, here.cover) * trapped *
           smoothstep(0.2, 0.5, here.source_beds);
}

// Pipes from deep under the oldest crust bring diamonds up, rarely.
double diamonds_at(const Here& here) {
    return here.light * smoothstep(4.0, 8.0, here.near) * here.exposed_core() *
           smoothstep(0.5, 0.7, here.pipes);
}

double kept(double likely, double abundance) {
    return std::clamp(likely * abundance, 0.0, 1.0);
}

} // namespace

Ores ores_on(const Sphere& sphere, const Crust& crust, const Bedrock& bedrock,
             const Outcrop& outcrop, const Climate& climate, const Rivers& rivers,
             const Lakes& lakes, const Sediment& sediment, const Soil& soil, const Body& body) {
    const std::uint32_t size = sphere.size();
    const int finest = std::max(2, std::min(sphere.subdivisions(), 6));
    const auto swamps = sphere_noise(sphere, hash_values(body.seed, 30), 2, finest);
    const auto source_beds = sphere_noise(sphere, hash_values(body.seed, 31), 2, finest);
    const auto salt_basins = sphere_noise(sphere, hash_values(body.seed, 32), 2, finest);
    const auto pipes = sphere_noise(sphere, hash_values(body.seed, 33), 3, finest);
    const auto veins = sphere_noise(sphere, hash_values(body.seed, 34), 3, finest);

    Ores ores;
    for (auto* field :
         {&ores.copper, &ores.gold, &ores.placers, &ores.banded_iron, &ores.laterite,
          &ores.bog_iron, &ores.lead_zinc, &ores.salt, &ores.coal, &ores.oil, &ores.diamonds}) {
        field->assign(size, 0.0);
    }
    const Body::OreAbundance& rich = body.ore_abundance;
    const double belt = std::max(body.belt_width, 1e-6);
    std::vector<double> heavy_source(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (in_sea(rivers, point)) {
            continue;
        }
        const Here here{
            .rock = outcrop.rock[point],
            .meeting = crust.meeting[point],
            .near = crust.boundary[point] / belt,
            .light = crust.light[point],
            .eroded = outcrop.eroded[point],
            .cover = outcrop.cover[point],
            .folding = bedrock.folding[point],
            .soluble = bedrock.soluble[point],
            .celsius = climate.temperature[point],
            .wet = wetness(climate.temperature[point], climate.rain[point], body),
            .wear = rivers.wear[point],
            .peat = soil.organic[point],
            .long_weathered =
                soil.parent[point] != Parent::None && soil.years[point] >= body.soil_years,
            .salt_flat = lakes.salt_flat[point] != 0,
            .swamps = swamps[point],
            .source_beds = source_beds[point],
            .salt_basins = salt_basins[point],
            .pipes = pipes[point],
            .veins = veins[point],
        };
        const double copper = copper_at(here);
        const double gold = gold_at(here);
        heavy_source[point] = std::max(copper, gold);
        ores.copper[point] = kept(copper, rich.copper);
        ores.gold[point] = kept(gold, rich.gold);
        ores.banded_iron[point] = kept(banded_iron_at(here), rich.banded_iron);
        ores.laterite[point] = kept(laterite_at(here), rich.laterite);
        ores.bog_iron[point] = kept(bog_iron_at(here), rich.bog_iron);
        ores.lead_zinc[point] = kept(lead_zinc_at(here), rich.lead_zinc);
        ores.salt[point] = kept(salt_at(here), rich.salt);
        ores.coal[point] = kept(coal_at(here), rich.coal);
        ores.oil[point] = kept(oil_at(here), rich.oil);
        ores.diamonds[point] = kept(diamonds_at(here), rich.diamonds);
    }
    const std::vector<double> placers = placed(rivers, sediment, heavy_source);
    for (std::uint32_t point = 0; point < size; ++point) {
        ores.placers[point] = kept(placers[point], rich.placers);
    }
    return ores;
}

} // namespace hog::formation
