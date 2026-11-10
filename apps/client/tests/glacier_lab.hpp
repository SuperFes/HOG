#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// A standalone test bed for glaciers: a small square-cell patch of mountain, a climate, and the
// processes that make ice (snow above the snowline, flow downhill, wear on the bed). Nothing here
// touches the globe, the generator or the servers.
namespace hog::client::lab {

struct Climate {
    double snowline = 380.0; // metres: balance is zero here
    double gain = 0.006;     // metres of ice a year per metre above the snowline
    double loss = 0.010;     // metres of ice a year per metre below it
    double most_gain = 2.0;  // snowfall caps out
    double most_loss = 8.0;  // melt caps out
    double flow = 2.2e-5;    // Glen's law, temperate ice: 2A (rho g)^3 / 5 in m^-3 per year
    double wear = 1.0e-5;    // metres of bed worn per metre of ice moved
    double sea = -1.0e9;     // sea level, metres: ice over deeper water floats off
    double calving = 0.5;    // share of floating ice lost to the sea a year
    double rockfall = 0.002; // metres of debris a year per metre of wall above the ice
    double englacial = 0.02; // share of melted ice that leaves its rock on the surface
    double insulation = 1.0; // debris depth, metres, that cuts melt by 1/e
};

struct Bed {
    int size = 0;               // cells along each side
    double cell = 10.0;         // metres per cell
    std::vector<double> rock;   // bed height, metres
    std::vector<double> ice;    // ice thickness over the bed, metres
    std::vector<double> debris; // rock lying on the ice, metres deep
    std::vector<double> floe;   // sea-ice thickness over open water, metres

    [[nodiscard]] std::size_t index(int x, int y) const noexcept {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
               static_cast<std::size_t>(x);
    }
    [[nodiscard]] double surface(int x, int y) const noexcept {
        return rock[index(x, y)] + ice[index(x, y)];
    }
};

[[nodiscard]] inline double hashed(int x, int y, unsigned seed) noexcept {
    auto h = static_cast<std::uint32_t>(x) * 374761393U +
             static_cast<std::uint32_t>(y) * 668265263U + seed * 2246822519U;
    h = (h ^ (h >> 13U)) * 1274126177U;
    return static_cast<double>(h ^ (h >> 16U)) / 4294967295.0;
}

[[nodiscard]] inline double value_noise(double x, double y, unsigned seed) noexcept {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double fx = x - ix;
    const double fy = y - iy;
    const double sx = fx * fx * (3.0 - (2.0 * fx));
    const double sy = fy * fy * (3.0 - (2.0 * fy));
    return std::lerp(std::lerp(hashed(ix, iy, seed), hashed(ix + 1, iy, seed), sx),
                     std::lerp(hashed(ix, iy + 1, seed), hashed(ix + 1, iy + 1, seed), sx), sy);
}

// Feature builders: each shapes the bed in place, so a test bed is a few calls. Positions are in
// cells; heights and sizes in metres.
[[nodiscard]] inline Bed flat(int size, double cell, double height) {
    Bed bed{.size = size, .cell = cell, .rock = {}, .ice = {}, .debris = {}, .floe = {}};
    bed.rock.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), height);
    bed.ice.assign(bed.rock.size(), 0.0);
    bed.debris.assign(bed.rock.size(), 0.0);
    bed.floe.assign(bed.rock.size(), 0.0);
    return bed;
}

// A rounded peak that rises `height` over what is there, `radius` metres wide.
inline void add_peak(Bed& bed, double cx, double cy, double radius, double height) {
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const double dx = (x - cx) * bed.cell / radius;
            const double dy = (y - cy) * bed.cell / radius;
            bed.rock[bed.index(x, y)] += height * std::exp(-((dx * dx) + (dy * dy)));
        }
    }
}

// A ridge line from one cell to another, `half_width` metres either side.
inline void add_ridge(Bed& bed, double x0, double y0, double x1, double y1, double half_width,
                      double height) {
    const double ax = x1 - x0;
    const double ay = y1 - y0;
    const double length2 = (ax * ax) + (ay * ay);
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const double t = std::clamp((((x - x0) * ax) + ((y - y0) * ay)) / length2, 0.0, 1.0);
            const double dx = (x - (x0 + (t * ax))) * bed.cell / half_width;
            const double dy = (y - (y0 + (t * ay))) * bed.cell / half_width;
            bed.rock[bed.index(x, y)] += height * std::exp(-((dx * dx) + (dy * dy)));
        }
    }
}

// A valley cut down `depth` metres along a line, as running water leaves it (V-shaped).
// `wander` (metres) bends the line sideways with noise, as a real valley meanders.
inline void add_valley(Bed& bed, double x0, double y0, double x1, double y1, double half_width,
                       double depth, double wander = 0.0, unsigned seed = 0) {
    const double ax = x1 - x0;
    const double ay = y1 - y0;
    const double length2 = (ax * ax) + (ay * ay);
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const double t = std::clamp((((x - x0) * ax) + ((y - y0) * ay)) / length2, 0.0, 1.0);
            const double bend = wander * ((2.0 * value_noise(t * 6.0, 0.5, seed)) - 1.0) / bed.cell;
            const double dx = (x - (x0 + (t * ax)) + (bend * -ay / std::sqrt(length2))) * bed.cell;
            const double dy = (y - (y0 + (t * ay)) + (bend * ax / std::sqrt(length2))) * bed.cell;
            const double across = std::sqrt((dx * dx) + (dy * dy)) / half_width;
            bed.rock[bed.index(x, y)] -= depth * std::max(1.0 - across, 0.0);
        }
    }
}

// Fractal noise of up to `amplitude` metres either way, features about `wavelength` metres across.
inline void roughen(Bed& bed, double amplitude, double wavelength, unsigned seed) {
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            double sum = 0.0;
            double weight = 1.0;
            double total = 0.0;
            for (unsigned octave = 0; octave < 4; ++octave) {
                const double f = bed.cell / (wavelength / static_cast<double>(1U << octave));
                sum += weight * ((2.0 * value_noise(x * f, y * f, seed + octave)) - 1.0);
                total += weight;
                weight *= 0.5;
            }
            bed.rock[bed.index(x, y)] += amplitude * sum / total;
        }
    }
}

// Tilts the whole bed so it falls by `drop` metres from the first column to the last.
inline void add_tilt(Bed& bed, double drop) {
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            bed.rock[bed.index(x, y)] -= drop * x / (bed.size - 1);
        }
    }
}

// A bowl `depth` metres deep, `radius` metres wide: a cirque, a lake basin or a sea inlet.
inline void add_basin(Bed& bed, double cx, double cy, double radius, double depth) {
    add_peak(bed, cx, cy, radius, -depth);
}

// A massif with its peak off-centre and ridged noise on its flanks, bare of ice.
[[nodiscard]] inline Bed mountain(int size, double cell, double peak, unsigned seed) {
    Bed bed{.size = size, .cell = cell, .rock = {}, .ice = {}, .debris = {}, .floe = {}};
    bed.rock.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    bed.ice.assign(bed.rock.size(), 0.0);
    bed.debris.assign(bed.rock.size(), 0.0);
    bed.floe.assign(bed.rock.size(), 0.0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double u = ((x + 0.5) / size) - 0.4;
            const double v = (y + 0.5) / size - 0.5;
            const double dome = std::exp(-((u * u) + (v * v)) * 9.0);
            double ridges = 0.0;
            double amplitude = 0.5;
            for (int octave = 0; octave < 4; ++octave) {
                const double f = 4.0 * std::pow(2.0, octave);
                ridges +=
                    amplitude *
                    (1.0 - std::abs((2.0 * value_noise(x * f / size, y * f / size,
                                                       seed + static_cast<unsigned>(octave))) -
                                    1.0));
                amplitude *= 0.5;
            }
            bed.rock[bed.index(x, y)] = peak * dome * (0.7 + (0.5 * ridges));
        }
    }
    return bed;
}

[[nodiscard]] inline double balance(const Climate& climate, double surface) noexcept {
    const double above = surface - climate.snowline;
    return above > 0.0 ? std::min(above * climate.gain, climate.most_gain)
                       : -std::min(-above * climate.loss, climate.most_loss);
}

// Moves ice downhill for up to `years` (shallow-ice: flux grows with thickness^5 and slope^2),
// wearing the bed where it moves. Returns the years actually stepped, which is shorter when the
// flow needs a finer step to stay stable.
inline double flow_step(Bed& bed, const Climate& climate, double years) {
    const auto n = static_cast<std::size_t>(bed.size);
    std::vector<double> flux_x(n * n, 0.0);
    std::vector<double> flux_y(n * n, 0.0);
    double stiffest = 0.0;
    const auto face = [&](int x0, int y0, int x1, int y1) {
        const double drop = (bed.surface(x0, y0) - bed.surface(x1, y1)) / bed.cell;
        // The cell ice leaves supplies the flux, so a bare cell gives none.
        const double thick = bed.ice[drop > 0.0 ? bed.index(x0, y0) : bed.index(x1, y1)];
        const double slope = std::abs(drop);
        const double stiffness = climate.flow * std::pow(thick, 5.0) * slope * slope;
        stiffest = std::max(stiffest, stiffness);
        return stiffness * drop;
    };
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            if (x + 1 < bed.size) {
                flux_x[bed.index(x, y)] = face(x, y, x + 1, y);
            }
            if (y + 1 < bed.size) {
                flux_y[bed.index(x, y)] = face(x, y, x, y + 1);
            }
        }
    }
    // Explicit diffusion is stable while a cell gives away less than a quarter of its neighbour
    // gap. Thick ice would force ever finer steps, so the step has a floor and each face's flux is
    // capped at a fifth of what its donor cell holds: still conservative and never negative, just
    // damped.
    const double dt =
        std::min(years, std::max(stiffest > 0.0 ? 0.2 * bed.cell / stiffest : years, 0.05));
    const auto cap = [&](std::vector<double>& flux, int dx, int dy) {
        for (int y = 0; y < bed.size - dy; ++y) {
            for (int x = 0; x < bed.size - dx; ++x) {
                double& f = flux[bed.index(x, y)];
                const double donor = bed.ice[f > 0.0 ? bed.index(x, y) : bed.index(x + dx, y + dy)];
                const double gap = std::abs(bed.surface(x, y) - bed.surface(x + dx, y + dy));
                const double most = 0.2 * std::min(donor, gap) * bed.cell / dt;
                f = std::clamp(f, -most, most);
            }
        }
    };
    cap(flux_x, 1, 0);
    cap(flux_y, 0, 1);
    // Debris rides the ice: each face carries the donor cell's share of it.
    std::vector<double> carried(n * n, 0.0);
    const auto share = [&](int x0, int y0, int x1, int y1, double flux) {
        const std::size_t donor = bed.index(flux > 0.0 ? x0 : x1, flux > 0.0 ? y0 : y1);
        return bed.ice[donor] > 0.0 ? flux * bed.debris[donor] / bed.ice[donor] : 0.0;
    };
    std::vector<double> debris_x(n * n, 0.0);
    std::vector<double> debris_y(n * n, 0.0);
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            if (x + 1 < bed.size) {
                debris_x[bed.index(x, y)] = share(x, y, x + 1, y, flux_x[bed.index(x, y)]);
            }
            if (y + 1 < bed.size) {
                debris_y[bed.index(x, y)] = share(x, y, x, y + 1, flux_y[bed.index(x, y)]);
            }
        }
    }
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const std::size_t i = bed.index(x, y);
            const double in_x = x > 0 ? flux_x[bed.index(x - 1, y)] : 0.0;
            const double in_y = y > 0 ? flux_y[bed.index(x, y - 1)] : 0.0;
            const double out = flux_x[i] + flux_y[i];
            const double moved = in_x + in_y - out;
            const double din_x = x > 0 ? debris_x[bed.index(x - 1, y)] : 0.0;
            const double din_y = y > 0 ? debris_y[bed.index(x, y - 1)] : 0.0;
            carried[i] = din_x + din_y - debris_x[i] - debris_y[i];
            bed.ice[i] = std::max(bed.ice[i] + (dt * moved / bed.cell), 0.0);
            bed.debris[i] = std::max(bed.debris[i] + (dt * carried[i] / bed.cell), 0.0);
            bed.rock[i] -= climate.wear * dt * (std::abs(in_x) + std::abs(in_y) + std::abs(out)) *
                           (bed.ice[i] > 0.0 ? 1.0 : 0.0);
        }
    }
    return dt;
}

// Snow falls or ice melts by the climate's balance at each cell's surface; ice never goes under 0.
inline void add_balance(Bed& bed, const Climate& climate, double years) {
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const std::size_t i = bed.index(x, y);
            double& ice = bed.ice[i];
            const double change = balance(climate, bed.surface(x, y));
            if (change < 0.0) {
                // Debris insulates what it covers, and melting leaves the ice's own rock behind.
                const double melt =
                    std::min(-change * std::exp(-bed.debris[i] / climate.insulation) * years, ice);
                ice -= melt;
                bed.debris[i] += climate.englacial * melt;
                if (ice <= 0.0) {
                    bed.rock[i] += bed.debris[i];
                    bed.debris[i] = 0.0;
                }
            } else {
                ice += years * change;
            }
        }
    }
}

// The patch's rim is open: ice reaching it is gone (a sea, or a valley carrying on out of frame).
inline void edges_drain(Bed& bed) {
    for (int i = 0; i < bed.size; ++i) {
        bed.ice[bed.index(i, 0)] = 0.0;
        bed.ice[bed.index(i, bed.size - 1)] = 0.0;
        bed.ice[bed.index(0, i)] = 0.0;
        bed.ice[bed.index(bed.size - 1, i)] = 0.0;
    }
}

// Runs the patch for `years`, a year of climate at a time with the flow stepped inside it.
inline void calve(Bed& bed, const Climate& climate) {
    for (std::size_t i = 0; i < bed.ice.size(); ++i) {
        const double depth = climate.sea - bed.rock[i];
        if (depth > 0.0 && bed.ice[i] * 0.917 < depth) {
            bed.ice[i] *= 1.0 - climate.calving;
            bed.debris[i] *= 1.0 - climate.calving;
        }
    }
}

// Loose rock falls from bare walls standing above the ice beside them.
inline void rockfall(Bed& bed, const Climate& climate, double years) {
    std::vector<double> fall(bed.ice.size(), 0.0);
    for (int y = 1; y + 1 < bed.size; ++y) {
        for (int x = 1; x + 1 < bed.size; ++x) {
            const std::size_t i = bed.index(x, y);
            if (bed.ice[i] <= 0.5) {
                continue;
            }
            for (const std::size_t j : {bed.index(x - 1, y), bed.index(x + 1, y),
                                        bed.index(x, y - 1), bed.index(x, y + 1)}) {
                if (bed.ice[j] <= 0.5) {
                    fall[i] += std::max(bed.rock[j] - (bed.rock[i] + bed.ice[i]), 0.0);
                }
            }
        }
    }
    for (std::size_t i = 0; i < fall.size(); ++i) {
        bed.debris[i] += climate.rockfall * years * fall[i];
    }
}

inline void run(Bed& bed, const Climate& climate, int years) {
    for (int year = 0; year < years; ++year) {
        add_balance(bed, climate, 1.0);
        rockfall(bed, climate, 1.0);
        edges_drain(bed);
        for (double left = 1.0; left > 1.0e-9;) {
            left -= flow_step(bed, climate, left);
        }
        calve(bed, climate);
    }
}

// As `run`, with the snowline moving `rise` metres every year (positive warms, negative cools).
inline void run_drifting(Bed& bed, Climate climate, int years, double rise) {
    for (int year = 0; year < years; ++year) {
        run(bed, climate, 1);
        climate.snowline += rise;
    }
}

// Ice thickness at each cell along a line, for reading a glacier's profile as numbers.
[[nodiscard]] inline std::vector<double> profile(const Bed& bed, int x0, int y0, int x1, int y1,
                                                 int samples) {
    std::vector<double> out;
    for (int i = 0; i < samples; ++i) {
        const double t = samples > 1 ? static_cast<double>(i) / (samples - 1) : 0.0;
        const int x = static_cast<int>(std::lround(std::lerp(x0, x1, t)));
        const int y = static_cast<int>(std::lround(std::lerp(y0, y1, t)));
        out.push_back(bed.ice[bed.index(x, y)]);
    }
    return out;
}

// Air temperature over the sea, degrees C: `base` at the first row, `per_row` colder each row on.
struct Cold {
    double base = -10.0;
    double per_row = 0.0;
    double patchy = 6.0;    // degrees of local noise (currents, shelter) either way
    double freezing = -1.8; // sea water freezes here
};

// One winter of sea ice: open water freezes next to ice or when it is far below freezing, ice
// thickens at a rate falling with its thickness (Stefan), and warmth melts it. Floes are where
// the local cold only just wins; a pack is where it wins everywhere.
inline void sea_ice_year(Bed& bed, const Climate& climate, const Cold& cold, unsigned seed) {
    std::vector<double> next = bed.floe;
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const std::size_t i = bed.index(x, y);
            if (bed.rock[i] >= climate.sea) {
                next[i] = 0.0;
                continue;
            }
            const double local =
                cold.base - (cold.per_row * y) +
                (cold.patchy * ((2.0 * value_noise(x * 0.25, y * 0.25, seed)) - 1.0));
            const double degrees = cold.freezing - local;
            double h = bed.floe[i];
            if (degrees > 0.0) {
                bool touching = false;
                for (const auto [dx, dy] :
                     {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int nx = std::clamp(x + dx, 0, bed.size - 1);
                    const int ny = std::clamp(y + dy, 0, bed.size - 1);
                    touching = touching || bed.floe[bed.index(nx, ny)] > 0.1 ||
                               bed.ice[bed.index(nx, ny)] > 0.5;
                }
                if (h > 0.1 || touching || degrees > 6.0) {
                    h = std::sqrt((h * h) + (0.04 * degrees));
                }
            } else {
                h = std::max(h + (0.04 * degrees), 0.0);
            }
            next[i] = std::min(h, 4.0);
        }
    }
    bed.floe = next;
}

[[nodiscard]] inline double sea_ice_cover(const Bed& bed, const Climate& climate) noexcept {
    int water = 0;
    int frozen = 0;
    for (std::size_t i = 0; i < bed.rock.size(); ++i) {
        if (bed.rock[i] < climate.sea) {
            ++water;
            frozen += bed.floe[i] > 0.1 ? 1 : 0;
        }
    }
    return water > 0 ? static_cast<double>(frozen) / water : 0.0;
}

[[nodiscard]] inline double total_debris(const Bed& bed) noexcept {
    double sum = 0.0;
    for (const double d : bed.debris) {
        sum += d;
    }
    return sum;
}

[[nodiscard]] inline double total_ice(const Bed& bed) noexcept {
    double sum = 0.0;
    for (const double ice : bed.ice) {
        sum += ice;
    }
    return sum;
}

// The lowest bed height under ice, i.e. how far down the glacier has reached.
[[nodiscard]] inline double terminus(const Bed& bed) noexcept {
    double lowest = 1.0e9;
    for (std::size_t i = 0; i < bed.ice.size(); ++i) {
        if (bed.ice[i] > 1.0) {
            lowest = std::min(lowest, bed.rock[i]);
        }
    }
    return lowest;
}

// A top-down picture of the bed as binary PPM: rock and ice hill-shaded from the north-west, ice
// tinted blue and deepening with its thickness.
inline void write_ppm(const Bed& bed, const std::string& path, int scale = 8,
                      double sea_level = -1.0e9) {
    std::ofstream out{path, std::ios::binary};
    const int pixels = bed.size * scale;
    out << "P6\n" << pixels << ' ' << pixels << "\n255\n";
    const auto at = [&](int x, int y) {
        return bed.surface(std::clamp(x, 0, bed.size - 1), std::clamp(y, 0, bed.size - 1));
    };
    for (int py = 0; py < pixels; ++py) {
        for (int px = 0; px < pixels; ++px) {
            const int x = px / scale;
            const int y = py / scale;
            const double gx = (at(x + 1, y) - at(x - 1, y)) / (2.0 * bed.cell);
            const double gy = (at(x, y + 1) - at(x, y - 1)) / (2.0 * bed.cell);
            const double norm = std::sqrt((gx * gx) + (gy * gy) + 1.0);
            const double light = 0.35 + (0.75 * std::max(((-gx - gy) * 0.5 + 0.7) / norm, 0.0));
            const double thick = bed.ice[bed.index(x, y)];
            const double height = std::clamp(bed.rock[bed.index(x, y)] / 800.0, 0.0, 1.0);
            double r = 0.35 + (0.25 * height);
            double g = 0.30 + (0.25 * height);
            double b = 0.25 + (0.25 * height);
            if (bed.rock[bed.index(x, y)] < sea_level) {
                r = 0.10;
                g = 0.25;
                b = 0.45;
                const double f = bed.floe[bed.index(x, y)];
                if (f > 0.1) {
                    r = g = b = std::lerp(0.75, 0.95, std::clamp(f / 2.0, 0.0, 1.0));
                }
            }
            if (thick > 0.5) {
                const double deep = std::clamp(thick / 150.0, 0.0, 1.0);
                r = std::lerp(0.80, 0.30, deep);
                g = std::lerp(0.88, 0.60, deep);
                b = std::lerp(0.95, 0.85, deep);
                const double cover = std::clamp(bed.debris[bed.index(x, y)] / 1.0, 0.0, 1.0);
                r = std::lerp(r, 0.30, cover);
                g = std::lerp(g, 0.25, cover);
                b = std::lerp(b, 0.20, cover);
            }
            const auto byte = [&](double v) {
                return static_cast<char>(
                    static_cast<unsigned char>(std::clamp(v * light, 0.0, 1.0) * 255.0));
            };
            const char rgb[3] = {byte(r), byte(g), byte(b)};
            out.write(static_cast<const char*>(rgb), 3);
        }
    }
}

} // namespace hog::client::lab
