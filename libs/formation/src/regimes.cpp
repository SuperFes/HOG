#include <hog/core/portable_maths.hpp>
#include <hog/formation/regimes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace hog::formation {

namespace {

constexpr double days_a_year = 365.25;
constexpr double seconds_a_year = days_a_year * 86'400.0;
constexpr double month_days = days_a_year / Regimes::months;
// The warmest of the year comes about a month after the longest day.
constexpr double summer_lag = 1.0 / 12.0;

double wrapped(double share) noexcept {
    return share - std::floor(share);
}

Months uniform() noexcept {
    Months even{};
    even.fill(1.0 / Regimes::months);
    return even;
}

Months normalised(Months series) noexcept {
    double sum = 0.0;
    for (const double month : series) {
        sum += month;
    }
    if (sum <= 0.0) {
        return uniform();
    }
    for (double& month : series) {
        month /= sum;
    }
    return series;
}

// One step of a lake spilling over its sill, taken implicitly so a small lake on a big river
// holds steady: the head over the sill at the step's end.
double spilled_head(double head, double net_in, double area, double weir, double step) {
    const double held = (area * head / step) + net_in;
    if (held <= 0.0) {
        return 0.0;
    }
    // Newton's way down the convex rise of what it holds with its head, from the head before.
    double next = std::max(head, maths::pow(held / weir, 2.0 / 3.0) * 1e-3);
    for (int iteration = 0; iteration < 12; ++iteration) {
        const double root = std::sqrt(next);
        const double over = (area * next / step) + (weir * next * root) - held;
        next = std::max(0.0, next - (over / ((area / step) + (1.5 * weir * root))));
    }
    return next;
}

} // namespace

double month_middle(int month) noexcept {
    return (month + 0.5) / Regimes::months;
}

double peak_of(const Months& series) noexcept {
    int most = 0;
    for (int month = 1; month < Regimes::months; ++month) {
        if (series.at(static_cast<std::size_t>(month)) >
            series.at(static_cast<std::size_t>(most))) {
            most = month;
        }
    }
    const auto at = [&](int month) {
        return series.at(static_cast<std::size_t>((month + Regimes::months) % Regimes::months));
    };
    // The parabola through it and its neighbours.
    const double before = at(most - 1);
    const double here = at(most);
    const double after = at(most + 1);
    const double bend = before - (2.0 * here) + after;
    const double offset = bend < 0.0 ? std::clamp(0.5 * (before - after) / bend, -0.5, 0.5) : 0.0;
    return wrapped(month_middle(most) + (offset / Regimes::months));
}

Months runoff_shares(double mean, double swing, double warmest, double rain, bool iced,
                     const Body& body) {
    Months temperature{};
    for (int month = 0; month < Regimes::months; ++month) {
        temperature.at(static_cast<std::size_t>(month)) =
            mean + (swing * maths::cos(2.0 * std::numbers::pi * (month_middle(month) - warmest)));
    }
    Months out{};
    if (iced) {
        for (std::size_t month = 0; month < out.size(); ++month) {
            out.at(month) = std::max(0.0, temperature.at(month) - body.freezing);
        }
        return normalised(out);
    }
    Months demand{};
    double mean_demand = 0.0;
    for (std::size_t month = 0; month < demand.size(); ++month) {
        demand.at(month) = open_water_loss(temperature.at(month), body);
        mean_demand += demand.at(month) / Regimes::months;
    }
    const double falls = (std::max(rain, 0.0) + mean_demand) / Regimes::months;
    // Round the year a few times, so the snow lying at its start is what the last year left.
    double snow = 0.0;
    for (int year = 0; year < 3; ++year) {
        for (std::size_t month = 0; month < out.size(); ++month) {
            const double left = std::max(0.0, falls - (demand.at(month) / Regimes::months));
            const double warmth = temperature.at(month) - body.freezing;
            if (warmth < 0.0) {
                snow += left;
                out.at(month) = 0.0;
            } else {
                const double melted = std::min(snow, body.melt_rate * warmth * month_days);
                snow -= melted;
                out.at(month) = left + melted;
            }
        }
    }
    return normalised(out);
}

Pooled pooled(const Months& in, const Months& loss, double area, bool open) {
    constexpr int steps = 30;
    const double step = seconds_a_year / (Regimes::months * steps);
    Months net{};
    double mean_net = 0.0;
    for (std::size_t month = 0; month < net.size(); ++month) {
        net.at(month) = in.at(month) - (loss.at(month) * area);
        mean_net += net.at(month) / Regimes::months;
    }
    Pooled made;
    if (!open || mean_net <= 0.0 || area <= 0.0) {
        // Closed, it rises and falls by what's left of the inflow, which the year round balances
        // what the air takes.
        double level = 0.0;
        for (std::size_t month = 0; month < net.size(); ++month) {
            level += (net.at(month) - mean_net) * step * steps / std::max(area, 1.0);
            made.level.at(month) = level;
        }
    } else {
        // Over a sill as wide as the river leaving it (as Channel has it at bankfull), as a
        // broad-crested weir.
        constexpr double weir_coefficient = 1.7;
        const double width = 3.0 * std::sqrt(mean_net) * maths::pow(4.0, 0.26);
        const double weir = weir_coefficient * width;
        double head = maths::pow(mean_net / weir, 2.0 / 3.0);
        // Until a year comes round the same.
        for (int year = 0; year < 4; ++year) {
            for (std::size_t month = 0; month < net.size(); ++month) {
                double level = 0.0;
                double out = 0.0;
                for (int at = 0; at < steps; ++at) {
                    head = spilled_head(head, net.at(month), area, weir, step);
                    level += head / steps;
                    out += weir * head * std::sqrt(head) / steps;
                }
                made.level.at(month) = level;
                made.out.at(month) = out;
            }
        }
    }
    const double lowest = *std::ranges::min_element(made.level);
    for (double& level : made.level) {
        level -= lowest;
    }
    return made;
}

namespace {

// Where a point is on the planet, for its year.
struct Place {
    const Sphere* sphere = nullptr;

    [[nodiscard]] double warmest(std::uint32_t point) const {
        return wrapped((sphere->point(point).z >= 0.0 ? 0.25 : 0.75) + summer_lag);
    }
    [[nodiscard]] double latitude(std::uint32_t point) const {
        return maths::asin(std::clamp(sphere->point(point).z, -1.0, 1.0));
    }
};

// Cubic metres a second each point gives its rivers each month from its own ground.
std::vector<Months> own_flows(const Place& place, const Climate& climate, const Rivers& rivers,
                              const Ice& ice, const Body& body) {
    const Sphere& sphere = *place.sphere;
    std::vector<Months> flow(sphere.size());
    const double cell_area = body.radius * body.radius;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue;
        }
        const double rain = std::max(climate.rain[point], 0.0);
        const double own = rain * sphere.area(point) * cell_area / seconds_a_year;
        const Months shares = runoff_shares(
            climate.temperature[point], seasonal_swing(place.latitude(point), false, body),
            place.warmest(point), rain, ice.thickness[point] > 0.0, body);
        for (std::size_t month = 0; month < shares.size(); ++month) {
            flow[point].at(month) = own * shares.at(month) * Regimes::months;
        }
    }
    return flow;
}

// Metres a second open water at a point loses to the air each month.
Months open_losses(const Place& place, std::uint32_t point, double celsius, const Body& body) {
    const double swing = seasonal_swing(place.latitude(point), true, body);
    Months loss{};
    for (int month = 0; month < Regimes::months; ++month) {
        const double warmth =
            celsius + (swing * maths::cos(2.0 * std::numbers::pi *
                                          (month_middle(month) - place.warmest(point))));
        loss.at(static_cast<std::size_t>(month)) = open_water_loss(warmth, body) / seconds_a_year;
    }
    return loss;
}

void river_regime(const Months& flow, std::uint32_t point, Regimes& regimes) {
    double sum = 0.0;
    for (const double month : flow) {
        sum += month;
    }
    if (sum > 0.0) {
        regimes.low[point] = *std::ranges::min_element(flow) * Regimes::months / sum;
        regimes.high[point] = *std::ranges::max_element(flow) * Regimes::months / sum;
        regimes.peak[point] = peak_of(flow);
    }
}

} // namespace

Regimes regimes_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers,
                   const Ice& ice, const Lakes& lakes, const Body& body) {
    const std::uint32_t size = sphere.size();
    const Place place{.sphere = &sphere};
    std::vector<Months> flow = own_flows(place, climate, rivers, ice, body);
    // Down the network, every point after all that run into it.
    std::vector<std::uint32_t> donors(size, 0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (rivers.receiver[point] != point) {
            ++donors[rivers.receiver[point]];
        }
    }
    std::vector<std::uint32_t> basin_out(size, Lakes::none);
    for (std::uint32_t basin = 0; basin < lakes.basins.size(); ++basin) {
        basin_out[lakes.basins[basin].outlet] = basin;
    }
    Regimes regimes;
    regimes.low.assign(size, 1.0);
    regimes.high.assign(size, 1.0);
    regimes.peak.assign(size, 0.0);
    regimes.lake_rise.assign(lakes.lakes.size(), 0.0);
    regimes.lake_peak.assign(lakes.lakes.size(), 0.0);
    std::vector<std::uint32_t> ready;
    for (std::uint32_t point = 0; point < size; ++point) {
        if (donors[point] == 0) {
            ready.push_back(point);
        }
    }
    while (!ready.empty()) {
        const std::uint32_t point = ready.back();
        ready.pop_back();
        Months& here = flow[point];
        // A hollow's water gathers at its outlet: a lake holds it back, a salt flat keeps it all.
        if (const std::uint32_t basin = basin_out[point]; basin != Lakes::none) {
            const std::uint32_t lake = lakes.basins[basin].lake;
            if (lake == Lakes::none) {
                here.fill(0.0);
            } else {
                const Pooled held =
                    pooled(here, open_losses(place, point, climate.temperature[point], body),
                           lakes.lakes[lake].area, lakes.lakes[lake].open);
                regimes.lake_rise[lake] = *std::ranges::max_element(held.level);
                regimes.lake_peak[lake] = peak_of(held.level);
                here = held.out;
            }
        }
        river_regime(here, point, regimes);
        const std::uint32_t down = rivers.receiver[point];
        if (down == point) {
            continue;
        }
        for (std::size_t month = 0; month < here.size(); ++month) {
            flow[down].at(month) += here.at(month);
        }
        if (--donors[down] == 0) {
            ready.push_back(down);
        }
    }
    return regimes;
}

} // namespace hog::formation
