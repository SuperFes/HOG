#include <hog/core/portable_maths.hpp>
#include <hog/formation/drifts.hpp>
#include <hog/formation/soil.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace hog::formation {

namespace {

// Reaches past which what's still carried is followed no further, laid where it's got to.
constexpr double reaches_followed = 4.0;

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

// Share of what's carried at a point that goes on a step downwind, going `reach` metres before
// most has settled.
double going_on(double step, double reach) {
    return reach > 0.0 ? maths::exp(-step / reach) : 0.0;
}

// Each point's neighbours downwind, and the share of what's carried on that goes to each.
struct Spread {
    std::vector<std::uint32_t> offsets; // into the two below, a point's from one to the next
    std::vector<std::uint32_t> next;
    std::vector<double> share;
};

Spread spread_downwind(const Sphere& sphere, const Climate& climate) {
    Spread spread;
    spread.offsets.reserve(sphere.size() + 1);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        spread.offsets.push_back(static_cast<std::uint32_t>(spread.next.size()));
        const DVec3 here = sphere.point(point);
        const auto first = spread.share.size();
        double sum = 0.0;
        for (const std::uint32_t next : sphere.neighbors(point)) {
            const double along =
                glm::dot(glm::normalize(sphere.point(next) - here), climate.wind[point]);
            if (along > 0.0) {
                // The more nearly downwind, the more: as the cosine to the fourth.
                const double weight = (along * along) * (along * along);
                spread.next.push_back(next);
                spread.share.push_back(weight);
                sum += weight;
            }
        }
        for (auto at = first; at < spread.share.size(); ++at) {
            spread.share[at] /= sum;
        }
    }
    spread.offsets.push_back(static_cast<std::uint32_t>(spread.next.size()));
    return spread;
}

// What's lifted at each point carried downwind a step at a time, a share going on from each
// point as `onward` says and the rest settling there; none kept where `lost`.
std::vector<double> carried(const Spread& spread, std::vector<double> load, int steps,
                            const std::function<double(std::uint32_t)>& onward,
                            const std::function<bool(std::uint32_t)>& lost) {
    const std::size_t size = load.size();
    std::vector<double> laid(size, 0.0);
    const auto lay = [&](std::uint32_t point, double amount) {
        if (!lost(point)) {
            laid[point] += amount;
        }
    };
    for (int step = 0; step < steps; ++step) {
        std::vector<double> next(size, 0.0);
        for (std::uint32_t point = 0; point < size; ++point) {
            if (load[point] <= 0.0) {
                continue;
            }
            const std::uint32_t from = spread.offsets[point];
            const std::uint32_t to = spread.offsets[point + 1];
            const double going = from == to ? 0.0 : onward(point);
            lay(point, load[point] * (1.0 - going));
            for (std::uint32_t at = from; at < to; ++at) {
                next[spread.next[at]] += load[point] * going * spread.share[at];
            }
        }
        load = std::move(next);
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        lay(point, load[point]);
    }
    return laid;
}

} // namespace

double dryness(double celsius, double rain, const Body& body) {
    return 1.0 - smoothstep(0.3, 1.0, wetness(celsius, rain, body));
}

double layered(double drift, const Body& body) {
    return drift >= body.thinnest_drift ? drift : 0.0;
}

std::vector<std::uint32_t> downwind(const Sphere& sphere, const Climate& climate) {
    std::vector<std::uint32_t> down(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const DVec3 here = sphere.point(point);
        double best = 0.0;
        down[point] = point;
        for (const std::uint32_t next : sphere.neighbors(point)) {
            const DVec3 way = sphere.point(next) - here;
            const double along = glm::dot(glm::normalize(way), climate.wind[point]);
            if (along > best) {
                best = along;
                down[point] = next;
            }
        }
    }
    return down;
}

Drifts drifts_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers, const Ice& ice,
                 const Lakes& lakes, const Sediment& sediment, const Body& body) {
    const std::uint32_t size = sphere.size();
    Drifts drifts;
    drifts.deflated.assign(size, 0.0);
    drifts.dunes.assign(size, 0.0);
    std::vector<double> bare(size, 0.0);
    std::vector<double> dry(size, 0.0);
    const auto drowned = [&](std::uint32_t point) {
        return in_sea(rivers, point) ||
               (lakes.lake[point] != Lakes::none && sediment.lake_depth[point] > 0.0);
    };
    const auto water = [&](std::uint32_t point) {
        return drowned(point) || ice.thickness[point] > 0.0;
    };
    std::vector<double> sand(size, 0.0);
    std::vector<double> silt(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (water(point)) {
            continue;
        }
        bare[point] = 1.0 - std::clamp(rivers.cover[point], 0.0, 1.0);
        dry[point] = dryness(climate.temperature[point], climate.rain[point], body);
        const double alluvium = std::min(sediment.alluvium[point], body.deflation_depth);
        const double river_laid = alluvium * bare[point] * dry[point] * body.deflation;
        // Ground ice left in the coldest times lay bare under its cold, dry winds.
        const bool ice_left = ice.coldest[point] > 0.0;
        const double ice_laid =
            ice_left ? std::min(ice.till[point], body.deflation_depth) * body.glacial_deflation
                     : 0.0;
        // Bare rock weathered over the ground's years, grain by grain, where nothing holds it.
        const double years = ice_left ? body.ice_left_years : body.soil_years;
        const double weathered =
            std::min(soil_making(climate.temperature[point], climate.rain[point], body) * years,
                     body.deflation_depth);
        const double rock_laid = weathered * bare[point] * dry[point] * body.rock_deflation;
        drifts.deflated[point] = river_laid + ice_laid + rock_laid;
        const double loose = river_laid + rock_laid;
        sand[point] = (loose * body.sand_share) + (ice_laid * body.sand_share / 3.0);
        silt[point] = (loose * body.silt_share) + (ice_laid * (1.0 - (body.sand_share / 3.0)));
        // The sea floor's deposits off a coast, blown ashore where the wind comes in off the sea.
        const DVec3 here = sphere.point(point);
        for (const std::uint32_t next : sphere.neighbors(point)) {
            if (!in_sea(rivers, next)) {
                continue;
            }
            const DVec3 ashore = glm::normalize(here - sphere.point(next));
            const double onshore = std::max(glm::dot(climate.wind[next], ashore), 0.0);
            sand[point] += std::min(sediment.delta[next], body.deflation_depth) * body.beach_sand *
                           onshore * bare[point];
        }
    }

    const std::vector<std::uint32_t> down = downwind(sphere, climate);
    const Spread spread = spread_downwind(sphere, climate);
    const double step = sphere.spacing() * body.radius;
    // Nothing goes further than halfway round the planet.
    const double furthest = std::numbers::pi / sphere.spacing();
    const auto steps = [&](double reach) {
        return static_cast<int>(std::ceil(std::min(reaches_followed * reach / step, furthest)));
    };
    // Sand goes on over bare, dry ground and ice, banking against ground rising ahead; it sinks
    // in water.
    const auto sand_onward = [&](std::uint32_t point) {
        if (drowned(point)) {
            return 0.0;
        }
        const std::uint32_t next = down[point];
        const double rise = in_sea(rivers, next)
                                ? 0.0
                                : std::max(rivers.height[next] - rivers.height[point], 0.0) / step;
        const double open = water(point) ? 1.0 : bare[point] * dry[point];
        return going_on(step,
                        body.sand_reach * open * (1.0 - smoothstep(0.0, body.climb_stop, rise)));
    };
    // Silt rides the air, settling sooner where plants catch it.
    const auto silt_onward = [&](std::uint32_t point) {
        const double caught = water(point) ? 0.0 : body.silt_trapped * (1.0 - bare[point]);
        return going_on(step, body.silt_reach * (1.0 - caught));
    };
    drifts.sand = carried(spread, std::move(sand), steps(body.sand_reach), sand_onward, water);
    drifts.loess = carried(spread, std::move(silt), steps(body.silt_reach), silt_onward, water);
    for (std::uint32_t point = 0; point < size; ++point) {
        drifts.dunes[point] = smoothstep(1.0, 10.0, drifts.sand[point]) *
                              smoothstep(0.3, 0.7, bare[point] * dry[point]);
    }
    return drifts;
}

} // namespace hog::formation
