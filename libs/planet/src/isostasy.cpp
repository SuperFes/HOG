#include <hog/core/assert.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace hog::planet {

namespace {

[[nodiscard]] std::int64_t to_fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

// Metres a column stands above a ridge's crest, ocean floor `cooled` of the way to its full depth.
[[nodiscard]] std::int64_t column_height(bool continental, double thickness, double cooled) {
    constexpr double metres_per_km = 1'000.0;
    if (continental) {
        return to_fixed(continent_freeboard +
                        (continental_buoyancy * metres_per_km *
                         (thickness - static_cast<double>(Tectonics::continental_thickness))));
    }
    return to_fixed((-cooled_depth * std::sqrt(cooled)) +
                    (oceanic_buoyancy * metres_per_km *
                     (thickness - static_cast<double>(Tectonics::oceanic_thickness))));
}

[[nodiscard]] double planet_area(const LevelGrid& grid) {
    double area = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        area += static_cast<double>(grid.area(cell));
    }
    return area;
}

} // namespace

std::vector<std::int64_t> crust_heights(const Tectonics& tectonics) {
    const auto columns = tectonics.columns();
    std::vector<std::int64_t> heights(columns.size());
    std::ranges::transform(columns, heights.begin(), [](const Tectonics::Column& column) {
        return column_height(column.crust == Tectonics::Crust::Continental,
                             static_cast<double>(column.thickness),
                             std::min(static_cast<double>(column.age) / cooling_steps, 1.0));
    });
    return heights;
}

std::vector<std::int64_t> crust_heights(const Lithosphere& lithosphere) {
    const LevelGrid& grid = lithosphere.grid();
    std::vector<std::int64_t> heights(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const std::optional<Lithosphere::Found> found =
            lithosphere.top(cell_center(grid.cell(cell)));
        const Crust crust = found ? found->crust : Crust{};
        heights[cell] =
            column_height(crust.type == CrustType::Continental,
                          static_cast<double>(crust.thickness),
                          std::min(static_cast<double>(crust.age) / cooling_years, 1.0)) -
            to_fixed(static_cast<double>(crust.sunk) * 1'000.0);
    }
    return heights;
}

std::vector<float> worn_down(const LevelGrid& grid, std::span<const std::int64_t> heights,
                             std::int64_t sea_level, double years, std::span<const Cover> cover) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    HOG_ASSERT(cover.size() == grid.size(), "one cover to each cell");
    constexpr double metres_per_km = 1'000.0;
    constexpr double buoyancy = continental_buoyancy;
    // Wearing off r kilometres lowers the land by buoyancy * r, so its height h over the ground
    // around falls as e^(-buoyancy * denudation * years), the faster through soft rock.
    const auto wearing = [](double above, double rate, double time) {
        return above / buoyancy * (1.0 - std::exp(-buoyancy * denudation * rate * time));
    };
    const auto through = [&](double above, const Cover& layers) {
        double worn = 0.0;
        double time = years;
        for (std::size_t kind = 0; kind < cover_kinds; ++kind) {
            const auto layer = static_cast<double>(layers[kind]);
            const double rate = softness[kind];
            if (buoyancy * layer >= above) {
                return worn + wearing(above, rate, time);
            }
            // How long the layer lasts, and what's under it wears down after.
            const double stripped =
                -std::log(1.0 - (buoyancy * layer / above)) / (buoyancy * denudation * rate);
            if (stripped >= time) {
                return worn + wearing(above, rate, time);
            }
            worn += layer;
            above -= buoyancy * layer;
            time -= stripped;
        }
        return worn + wearing(above, 1.0, time);
    };
    std::vector<float> worn(heights.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        std::int64_t base = heights[cell];
        for (const std::uint32_t next : grid.neighbors(cell)) {
            base = std::min(base, heights[next]);
        }
        base = std::max(base, sea_level);
        const double above = std::max(fixed_to_double(heights[cell] - base), 0.0) / metres_per_km;
        worn[cell] = static_cast<float>(through(above, cover[cell]));
    }
    return worn;
}

std::vector<bool> drowned(std::span<const std::int64_t> heights, std::int64_t sea_level) {
    std::vector<bool> under(heights.size());
    for (std::size_t cell = 0; cell < heights.size(); ++cell) {
        under[cell] = heights[cell] < sea_level;
    }
    return under;
}

std::vector<std::int64_t> flexed(const LevelGrid& grid, std::span<const std::int64_t> heights) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    // Each pass spreads a height over about a cell either way, each pair of neighbours trading
    // alike so that nothing is lost.
    constexpr double share = 1.0 / 9.0;
    const double spacing = std::sqrt(planet_area(grid) / static_cast<double>(grid.size()));
    const auto passes = static_cast<int>(std::lround(std::pow(flexure_reach / spacing, 2.0)));
    std::vector<double> spread(heights.size());
    std::ranges::transform(heights, spread.begin(),
                           [](std::int64_t height) { return fixed_to_double(height); });
    std::vector<double> next(heights.size());
    for (int pass = 0; pass < passes; ++pass) {
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            double traded = 0.0;
            for (const std::uint32_t neighbor : grid.neighbors(cell)) {
                traded += spread[neighbor] - spread[cell];
            }
            next[cell] = spread[cell] + (share * traded);
        }
        std::swap(spread, next);
    }
    std::vector<std::int64_t> bent(heights.size());
    std::ranges::transform(spread, bent.begin(), to_fixed);
    return bent;
}

double elastic_thickness(const Crust& crust) {
    // Continents are heated again where they're piled up into ranges, and ocean floor as it forms.
    const bool continental = crust.type == CrustType::Continental;
    const auto cooled = static_cast<double>(continental ? crust.raised : crust.age);
    return std::min(elastic_deepening * std::sqrt(std::max(cooled, 0.0)),
                    (continental ? continental_plate : oceanic_plate) / 3.0);
}

double stiffness(double elastic, double radius) {
    constexpr double earth_radius = 6'371'000.0;
    constexpr double metres_per_km = 1'000.0;
    const double thick = elastic * metres_per_km;
    const double rigidity =
        young_modulus * thick * thick * thick / (12.0 * (1.0 - (poisson_ratio * poisson_ratio)));
    return rigidity / (mantle_density * earth_gravity) * std::pow(radius / earth_radius, 4.0);
}

namespace {

// A plate bending as D ∇⁴w + ρg w = ρg load, as (M + K C M⁻¹ K) w = M load over the grid: M each
// place's area, C its stiffness, and K the Laplacian's stiffness, a third between each pair of
// neighbours, since eight of them round a square spread a field as h² ∇² over three.
class Bending {
public:
    Bending(const LevelGrid& grid, std::span<const double> stiffness,
            std::span<const std::uint32_t> plates)
        : grid_{&grid}, stiffness_{stiffness}, plates_{plates}, area_(grid.size()),
          curved_(grid.size()) {
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            area_[cell] = static_cast<double>(grid.area(cell));
        }
    }

    [[nodiscard]] double area(std::uint32_t cell) const { return area_[cell]; }

    void apply(std::span<const double> bent, std::span<double> out) {
        curve(bent, curved_);
        for (std::uint32_t cell = 0; cell < grid_->size(); ++cell) {
            curved_[cell] *= stiffness_[cell] / area_[cell];
        }
        curve(curved_, out);
        for (std::uint32_t cell = 0; cell < grid_->size(); ++cell) {
            out[cell] += area_[cell] * bent[cell];
        }
    }

    [[nodiscard]] std::vector<double> diagonal() const {
        constexpr double link = 1.0 / 3.0;
        std::vector<double> diagonal(grid_->size());
        for (std::uint32_t cell = 0; cell < grid_->size(); ++cell) {
            double links = 0.0;
            double around = 0.0;
            for (const std::uint32_t next : grid_->neighbors(cell)) {
                if (plates_[next] == plates_[cell]) {
                    links += link;
                    around += link * link * stiffness_[next] / area_[next];
                }
            }
            diagonal[cell] =
                area_[cell] + (links * links * stiffness_[cell] / area_[cell]) + around;
        }
        return diagonal;
    }

private:
    void curve(std::span<const double> field, std::span<double> out) const {
        for (std::uint32_t cell = 0; cell < grid_->size(); ++cell) {
            double sum = 0.0;
            for (const std::uint32_t next : grid_->neighbors(cell)) {
                if (plates_[next] == plates_[cell]) {
                    sum += field[cell] - field[next];
                }
            }
            out[cell] = sum / 3.0;
        }
    }

    const LevelGrid* grid_;
    std::span<const double> stiffness_;
    std::span<const std::uint32_t> plates_;
    std::vector<double> area_;
    std::vector<double> curved_;
};

[[nodiscard]] double dot(std::span<const double> one, std::span<const double> other) {
    double sum = 0.0;
    for (std::size_t index = 0; index < one.size(); ++index) {
        sum += one[index] * other[index];
    }
    return sum;
}

} // namespace

std::vector<double> deflected(const LevelGrid& grid, std::span<const double> loads,
                              std::span<const double> stiffness,
                              std::span<const std::uint32_t> plates) {
    HOG_ASSERT(loads.size() == grid.size(), "one load to each cell");
    HOG_ASSERT(stiffness.size() == grid.size(), "one stiffness to each cell");
    HOG_ASSERT(plates.size() == grid.size(), "one plate to each cell");
    constexpr double tolerance = 1e-10;
    const std::size_t size = grid.size();
    Bending bending{grid, stiffness, plates};
    // Conjugate gradients from where each place would sink alone, preconditioned by the diagonal.
    std::vector<double> bent(loads.begin(), loads.end());
    std::vector<double> residual(size);
    bending.apply(bent, residual);
    double wanted = 0.0;
    for (std::uint32_t cell = 0; cell < size; ++cell) {
        const double load = bending.area(cell) * loads[cell];
        residual[cell] = load - residual[cell];
        wanted += load * load;
    }
    const std::vector<double> diagonal = bending.diagonal();
    std::vector<double> step(size);
    std::ranges::transform(residual, diagonal, step.begin(), std::divides{});
    double along = dot(residual, step);
    std::vector<double> pushed(size);
    std::vector<double> scaled(size);
    for (std::size_t pass = 0;
         pass < size && dot(residual, residual) > tolerance * tolerance * wanted; ++pass) {
        bending.apply(step, pushed);
        const double length = along / dot(step, pushed);
        for (std::size_t cell = 0; cell < size; ++cell) {
            bent[cell] += length * step[cell];
            residual[cell] -= length * pushed[cell];
        }
        std::ranges::transform(residual, diagonal, scaled.begin(), std::divides{});
        const double next = dot(residual, scaled);
        for (std::size_t cell = 0; cell < size; ++cell) {
            step[cell] = scaled[cell] + (next / along * step[cell]);
        }
        along = next;
    }
    return bent;
}

double water_below(const LevelGrid& grid, std::span<const std::int64_t> heights,
                   std::int64_t level) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    double water = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (heights[cell] < level) {
            water += static_cast<double>(grid.area(cell)) * fixed_to_double(level - heights[cell]);
        }
    }
    return water;
}

std::int64_t sea_level(const LevelGrid& grid, std::span<const std::int64_t> heights, double water) {
    HOG_ASSERT(!heights.empty(), "ground to fill");
    // Water deep enough to cover the highest ground evenly is enough to fill any of it.
    std::int64_t low = std::ranges::min(heights);
    std::int64_t high = std::ranges::max(heights) + to_fixed(water / planet_area(grid)) + 1;
    while (low < high) {
        const std::int64_t middle = low + ((high - low) / 2);
        if (water_below(grid, heights, middle) < water) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::int64_t level_covering(const LevelGrid& grid, std::span<const std::int64_t> heights,
                            double fraction) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    HOG_ASSERT(fraction >= 0.0 && fraction <= 1.0, "fraction out of range");
    std::vector<std::uint32_t> order(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        order[cell] = cell;
    }
    std::ranges::sort(order, [&](std::uint32_t one, std::uint32_t other) {
        return heights[one] != heights[other] ? heights[one] < heights[other] : one < other;
    });
    const double covered = fraction * planet_area(grid);
    double below = 0.0;
    for (const std::uint32_t cell : order) {
        below += static_cast<double>(grid.area(cell));
        if (below > covered) {
            return heights[cell];
        }
    }
    return heights[order.back()] + 1;
}

double starting_water(const LevelGrid& grid) {
    return water_layer * planet_area(grid);
}

double topped_up(const LevelGrid& grid, std::span<const std::int64_t> heights, double water,
                 double ocean_fraction) {
    return std::max(water,
                    water_below(grid, heights, level_covering(grid, heights, ocean_fraction)));
}

int settling_steps(const Tectonics::Settings& settings) {
    const double cells = face_count * std::ldexp(1.0, 2 * settings.level);
    const double cell_angle = std::sqrt(4.0 * std::numbers::pi / cells);
    return static_cast<int>(std::lround(settling_turn / (settings.travel * cell_angle)));
}

Settled settle(std::uint64_t seed, double radius, Tectonics::Settings settings,
               double ocean_fraction) {
    return settle(seed, radius, settings, ocean_fraction, settling_steps(settings));
}

Settled settle(std::uint64_t seed, double radius, Tectonics::Settings settings,
               double ocean_fraction, int steps) {
    Settled settled{.tectonics = Tectonics{seed, radius, settings}, .heights = {}};
    for (int step = 0; step < steps; ++step) {
        settled.tectonics.step();
    }
    const LevelGrid& grid = settled.tectonics.grid();
    settled.heights = flexed(grid, crust_heights(settled.tectonics));
    settled.sea_level =
        sea_level(grid, settled.heights,
                  topped_up(grid, settled.heights, starting_water(grid), ocean_fraction));
    return settled;
}

double continental_share(double ocean_fraction) {
    return std::clamp(1.0 - ocean_fraction + shelf_share, 0.05, 0.95);
}

} // namespace hog::planet
