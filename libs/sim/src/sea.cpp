#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/sim/sea.hpp>
#include <hog/sim/weather.hpp>
#include <hog/waves/waves.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace hog::sim {

namespace {

using detail::earth_radius;

constexpr double g = waves::gravity;
constexpr double tau = 2.0 * std::numbers::pi;
constexpr double heading_angle = tau / swell_headings;

// JONSWAP's seas, grown over X metres of a wind of U: their variance·g²/U⁴ is growth·χ and their
// peak·U/g is peak_scale·χ^-peak_power, with χ = gX/U².
constexpr double growth = 1.6e-7;
constexpr double peak_scale = tau * 3.5;
constexpr double peak_power = 0.33;
// A sea following its peak at the peak's group speed, g/2ω, grows over time as it would over the
// distance it crossed: gt/U = duration_scale·χ^(1 - peak_power).
constexpr double duration_scale = 2.0 * peak_scale / (1.0 - peak_power);

// The middle of a band, rad/s.
double band_frequency(int band) {
    return std::sqrt(waves::band_lowest(band) * waves::band_lowest(band + 1));
}

double group_speed(double frequency) {
    return g / (2.0 * frequency);
}

DVec2 heading_of(int heading) {
    const double angle = heading_angle * heading;
    return {std::cos(angle), std::sin(angle)};
}

std::size_t bin(std::size_t cell, int band, int heading) {
    return (cell * swell_bins) + static_cast<std::size_t>((band * swell_headings) + heading);
}

// The most a wind of `speed` m/s raises.
double grown_variance(double speed) {
    const double height = waves::grown_sea(DVec3{speed, 0.0, 0.0}).height;
    return height * height / 16.0;
}

double fetch_of(double variance, double speed) {
    return variance * g * g / (growth * std::pow(speed, 4.0));
}

double peak_of(double variance, double speed) {
    if (variance <= 0.0 || speed <= 0.0) {
        return 0.0;
    }
    const double young = peak_scale * g / speed * std::pow(fetch_of(variance, speed), -peak_power);
    return std::max(young, waves::grown_sea(DVec3{speed, 0.0, 0.0}).peak);
}

// A wind sea of `variance` grown on by `seconds` more of a wind of `speed`.
double grown_on(double variance, double speed, double seconds) {
    const double unit = duration_scale * speed / g;
    const double since = (unit * std::pow(fetch_of(variance, speed), 1.0 - peak_power)) + seconds;
    const double fetch = std::pow(since / unit, 1.0 / (1.0 - peak_power));
    return std::min(growth * fetch * std::pow(speed, 4.0) / (g * g), grown_variance(speed));
}

// A band's swell at a heading `at` headings round, between the two it falls between.
double between(std::span<const float> band, double at) {
    const double round = at - (std::floor(at / swell_headings) * swell_headings);
    const int whole = std::min(static_cast<int>(round), swell_headings - 1);
    const double part = round - whole;
    const auto first = static_cast<std::size_t>(whole);
    const auto second = static_cast<std::size_t>((whole + 1) % swell_headings);
    return (static_cast<double>(band[first]) * (1.0 - part)) +
           (static_cast<double>(band[second]) * part);
}

} // namespace

SeaFields::SeaFields(std::size_t size)
    : wind_sea(size), peak(size), heading(size, Vec2{1.0F, 0.0F}), swell(size * swell_bins) {}

std::span<const float> SeaFields::swell_at(std::size_t index) const {
    return std::span<const float>{swell}.subspan(index * swell_bins, swell_bins);
}

std::array<WaveSystem, 2> strongest_swells(std::span<const float> swell) {
    std::array<double, swell_headings> by_heading{};
    for (int band = 0; band < swell_bands; ++band) {
        for (int heading = 0; heading < swell_headings; ++heading) {
            by_heading.at(static_cast<std::size_t>(heading)) +=
                static_cast<double>(swell[bin(0, band, heading)]);
        }
    }
    std::array<bool, swell_headings> taken{};
    std::array<WaveSystem, 2> found{};
    for (WaveSystem& system : found) {
        std::size_t best = swell_headings;
        double most = 0.0;
        for (std::size_t heading = 0; heading < by_heading.size(); ++heading) {
            if (!taken.at(heading) && by_heading.at(heading) > most) {
                most = by_heading.at(heading);
                best = heading;
            }
        }
        if (best == swell_headings) {
            break;
        }
        double variance = 0.0;
        DVec2 way{0.0};
        std::array<double, swell_bands> by_band{};
        for (const std::size_t heading :
             {(best + swell_headings - 1) % swell_headings, best, (best + 1) % swell_headings}) {
            if (taken.at(heading)) {
                continue;
            }
            taken.at(heading) = true;
            variance += by_heading.at(heading);
            way += heading_of(static_cast<int>(heading)) * by_heading.at(heading);
            for (int band = 0; band < swell_bands; ++band) {
                by_band.at(static_cast<std::size_t>(band)) +=
                    static_cast<double>(swell[bin(0, band, static_cast<int>(heading))]);
            }
        }
        // The peak, between the band holding most and its neighbours, along a parabola.
        const auto top =
            static_cast<std::size_t>(std::ranges::max_element(by_band) - by_band.begin());
        double off = 0.0;
        if (top > 0 && top + 1 < by_band.size()) {
            const double below = by_band.at(top - 1);
            const double above = by_band.at(top + 1);
            const double bend = below - (2.0 * by_band.at(top)) + above;
            if (bend < 0.0) {
                off = std::clamp(0.5 * (below - above) / bend, -0.5, 0.5);
            }
        }
        const double ratio = waves::band_lowest(1) / waves::band_lowest(0);
        system = {.height = static_cast<float>(4.0 * std::sqrt(variance)),
                  .peak = static_cast<float>(band_frequency(static_cast<int>(top)) *
                                             std::pow(ratio, off)),
                  .heading = Vec2{glm::normalize(way)}};
    }
    return found;
}

Sea::Sea(const CellGrid& grid, std::span<const Ground> ground, const SeaTuning& tuning)
    : tuning_(tuning), fields_(grid.size()), before_(grid.size()) {
    HOG_ASSERT(ground.size() == grid.size(), "need ground for every cell");
    wet_.reserve(grid.size());
    turns_.reserve(grid.size());
    double nearest = std::numeric_limits<double>::max();
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        wet_.push_back(ground[index].ocean);
        const planet::TangentFrame& here = grid.frame(index);
        std::array<double, 4> turns{};
        for (std::size_t k = 0; k < turns.size(); ++k) {
            const planet::TangentFrame& there = grid.frame(grid.neighbors(index)[k]);
            const DVec3 east =
                glm::normalize(here.east - (there.up * glm::dot(here.east, there.up)));
            turns.at(k) =
                std::atan2(glm::dot(east, there.north), glm::dot(east, there.east)) / heading_angle;
            nearest = std::min(nearest, glm::length(grid.offsets(index)[k]));
        }
        turns_.push_back(turns);
    }
    longest_step_ = 0.5 * nearest * earth_radius / group_speed(band_frequency(0));
}

void Sea::set_fields(const CellGrid& grid, SeaFields fields) {
    HOG_ASSERT(fields.size() == grid.size() && fields.peak.size() == grid.size() &&
                   fields.heading.size() == grid.size() &&
                   fields.swell.size() == grid.size() * swell_bins,
               "need the sea for every cell");
    fields_ = std::move(fields);
}

void Sea::raise(const CellGrid& grid, std::span<const Vec2> wind, double seconds) {
    HOG_ASSERT(wind.size() == grid.size(), "need wind for every cell");
    fields_ = SeaFields{grid.size()};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const DVec2 blowing{wind[index]};
        const double speed = glm::length(blowing);
        if (wet_[index] && speed > 0.0) {
            const double variance = grown_on(0.0, speed, seconds);
            fields_.wind_sea[index] = static_cast<float>(variance);
            fields_.peak[index] = static_cast<float>(peak_of(variance, speed));
            fields_.heading[index] = Vec2{blowing / speed};
        }
    }
}

void Sea::step(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
               jobs::JobSystem* jobs) {
    HOG_ASSERT(wind.size() == grid.size(), "need wind for every cell");
    const double steps = std::max(std::ceil(seconds / longest_step_), 1.0);
    for (double step = 0.0; step < steps; step += 1.0) {
        run_swell(grid, seconds / steps, jobs);
        run_wind_sea(grid, wind, seconds / steps, jobs);
    }
}

WaveSystem Sea::wind_sea(std::uint32_t index) const {
    return {.height = 4.0F * std::sqrt(fields_.wind_sea[index]),
            .peak = fields_.peak[index],
            .heading = fields_.heading[index]};
}

void Sea::run_swell(const CellGrid& grid, double seconds, jobs::JobSystem* jobs) {
    before_.swell = fields_.swell;
    // How far each band runs in radians, and how much of it is left after, steeper swell
    // wearing away sooner.
    std::array<double, swell_bands> reach{};
    std::array<double, swell_bands> kept{};
    for (int band = 0; band < swell_bands; ++band) {
        const double frequency = band_frequency(band);
        const double speed = group_speed(frequency);
        const double longer = 0.5 / frequency;
        const auto at = static_cast<std::size_t>(band);
        reach.at(at) = speed * seconds / earth_radius;
        kept.at(at) =
            std::exp(-speed * seconds / (tuning_.swell_reach * longer * longer * longer * longer));
    }
    const std::span<const float> before{before_.swell};
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        if (!wet_[index]) {
            std::fill_n(fields_.swell.begin() + static_cast<std::ptrdiff_t>(bin(index, 0, 0)),
                        swell_bins, 0.0F);
            return;
        }
        // Carried in from where it was, along great circles: as interpolate() would read it at
        // the departure point, each neighbour's swell turned into this cell's headings, and
        // none from land.
        for (int band = 0; band < swell_bands; ++band) {
            const auto at = static_cast<std::size_t>(band);
            for (int heading = 0; heading < swell_headings; ++heading) {
                const DVec2 way = heading_of(heading);
                double toward = 0.0;
                double total = 0.0;
                for (std::size_t k = 0; k < 4; ++k) {
                    const DVec2 to = grid.offsets(index)[k];
                    const double share =
                        std::clamp(-reach.at(at) * glm::dot(way, to) / glm::dot(to, to), 0.0, 1.0);
                    const std::uint32_t there = grid.neighbors(index)[k];
                    if (share <= 0.0) {
                        continue;
                    }
                    toward += share;
                    if (wet_[there]) {
                        total +=
                            share * between(before.subspan(bin(there, band, 0), swell_headings),
                                            heading + turns_[index][k]);
                    }
                }
                const double stay = std::max(1.0 - toward, 0.0);
                const std::size_t here = bin(index, band, heading);
                fields_.swell[here] =
                    static_cast<float>((total + (stay * static_cast<double>(before[here]))) /
                                       (toward + stay) * kept.at(at));
            }
        }
    });
}

void Sea::run_wind_sea(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
                       jobs::JobSystem* jobs) {
    before_.wind_sea = fields_.wind_sea;
    before_.peak = fields_.peak;
    before_.heading = fields_.heading;
    const double absorbing = std::exp(-seconds / tuning_.absorbing.count());
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        if (!wet_[index]) {
            fields_.wind_sea[index] = 0.0F;
            fields_.peak[index] = 0.0F;
            return;
        }
        auto peak = static_cast<double>(before_.peak[index]);
        DVec2 heading{before_.heading[index]};
        double variance = static_cast<double>(before_.wind_sea[index]);
        // Carried along at the group speed of its peak; into a calm cell, at that of the sea
        // around it, so it runs on out of the wind that raised it, and is left there as swell.
        const planet::TangentFrame& frame = grid.frame(index);
        DVec3 moving{0.0};
        double weight = 0.0;
        const auto add = [&](std::uint32_t cell) {
            const auto from = static_cast<double>(before_.peak[cell]);
            if (!wet_[cell] || from <= 0.0) {
                return;
            }
            const planet::TangentFrame& there = grid.frame(cell);
            const Vec2 way = before_.heading[cell];
            const auto amount = static_cast<double>(before_.wind_sea[cell]);
            moving += ((there.east * static_cast<double>(way.x)) +
                       (there.north * static_cast<double>(way.y))) *
                      (group_speed(from) * amount);
            weight += amount;
        };
        add(index);
        if (weight == 0.0) {
            for (const std::uint32_t there : grid.neighbors(index)) {
                add(there);
            }
        }
        const DVec3 velocity = weight > 0.0 ? moving / weight : DVec3{0.0};
        const DVec2 along{glm::dot(velocity, frame.east), glm::dot(velocity, frame.north)};
        const double fast = glm::length(along);
        if (fast > 0.0) {
            if (peak <= 0.0) {
                peak = g / (2.0 * fast);
                heading = along / fast;
            }
            const DVec3 from = detail::departure(frame, Vec2{along}, seconds);
            variance = grid.interpolate(from, [&](std::uint32_t cell) {
                return wet_[cell] ? static_cast<double>(before_.wind_sea[cell]) : 0.0;
            });
        }
        const DVec2 blowing{wind[index]};
        const double speed = glm::length(blowing);
        const DVec2 way = speed > 0.0 ? blowing / speed : heading;
        // As much as the wind holds up along the sea's heading; the rest outruns it, or is left
        // running across it, as swell.
        const double held = grown_variance(speed * std::max(glm::dot(heading, way), 0.0));
        if (variance > held) {
            if (peak > 0.0) {
                const waves::System left{.heading = DVec3{heading, 0.0},
                                         .height = 4.0 * std::sqrt(variance - held),
                                         .peak = peak,
                                         .peakedness = waves::wind_sea_peakedness,
                                         .spread = 2.0};
                const auto banded = waves::band_variances(left);
                std::array<double, swell_headings> spread{};
                double spreads = 0.0;
                for (int at = 0; at < swell_headings; ++at) {
                    const double facing = std::max(glm::dot(heading_of(at), heading), 0.0);
                    spread.at(static_cast<std::size_t>(at)) = facing * facing;
                    spreads += facing * facing;
                }
                for (int band = 0; band < swell_bands; ++band) {
                    for (int at = 0; at < swell_headings; ++at) {
                        fields_.swell[bin(index, band, at)] +=
                            static_cast<float>(banded.at(static_cast<std::size_t>(band)) *
                                               spread.at(static_cast<std::size_t>(at)) / spreads);
                    }
                }
            }
            variance = held;
        }
        if (speed <= 0.0) {
            fields_.wind_sea[index] = 0.0F;
            fields_.peak[index] = 0.0F;
            return;
        }
        variance = grown_on(variance, speed, seconds);
        const double grown_peak = peak_of(variance, speed);
        fields_.wind_sea[index] = static_cast<float>(variance);
        fields_.peak[index] = static_cast<float>(grown_peak);
        fields_.heading[index] = Vec2{way};
        // Swell running with the wind no longer than the wind sea's own waves is soon part of
        // it, which the wind already holds as high as it can.
        for (int band = 0; band < swell_bands; ++band) {
            if (band_frequency(band) < grown_peak) {
                continue;
            }
            for (int at = 0; at < swell_headings; ++at) {
                if (glm::dot(heading_of(at), way) > 0.0) {
                    fields_.swell[bin(index, band, at)] *= static_cast<float>(absorbing);
                }
            }
        }
    });
}

} // namespace hog::sim
