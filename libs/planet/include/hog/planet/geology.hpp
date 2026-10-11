#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/planet_config.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace hog::planet {

enum class Rock : std::uint8_t {
    Soil,
    Sand,
    Sandstone,
    Shale,
    Limestone,
    Granite,
    Basalt,
};

inline constexpr int rock_count = static_cast<int>(Rock::Basalt) + 1;

[[nodiscard]] std::string_view to_string(Rock rock) noexcept;

// A cell's ground from the surface down: loose cover, then sedimentary rock, then bedrock that
// runs on without end. Thicknesses are fixed-point metres and may be zero.
struct Strata {
    Rock cover = Rock::Soil;
    std::int64_t cover_thickness = 0;
    Rock sediment = Rock::Shale;
    std::int64_t sediment_thickness = 0;
    Rock bedrock = Rock::Granite;

    // The rock at a fixed-point depth in metres below the surface.
    [[nodiscard]] constexpr Rock rock_at(std::int64_t depth) const noexcept {
        if (depth < cover_thickness) {
            return cover;
        }
        if (depth < cover_thickness + sediment_thickness) {
            return sediment;
        }
        return bedrock;
    }

    // Fixed-point metres below the surface to the top of the bedrock.
    [[nodiscard]] constexpr std::int64_t bedrock_depth() const noexcept {
        return cover_thickness + sediment_thickness;
    }
};

enum class Resource : std::uint8_t {
    Clay,
    Coal,
    Salt,
    Iron,
    Copper,
    Tin,
    Gold,
};

inline constexpr int resource_count = static_cast<int>(Resource::Gold) + 1;

[[nodiscard]] std::string_view to_string(Resource resource) noexcept;

// A flat seam under a cell, in fixed-point metres below the surface; no seam has zero thickness.
// What a cell holds is the seam's thickness times the cell's area, so it is finite at every level.
struct Deposit {
    std::int64_t top = 0;
    std::int64_t thickness = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return thickness == 0; }
};

// Rock layers and ore deposits from terrain and climate: continents sit on granite under a cover
// of sedimentary rock that mountains strip away; ocean floors and the starting islands are basalt.
class Geology {
public:
    explicit Geology(const PlanetConfig& config);

    [[nodiscard]] const Climate& climate() const noexcept { return climate_; }
    [[nodiscard]] const Elevation& terrain() const noexcept { return climate_.terrain(); }

    [[nodiscard]] Strata strata(CellId cell) const;

    // Every resource's seam under the cell, indexed by resource. Only land holds deposits.
    [[nodiscard]] std::array<Deposit, resource_count> deposits(CellId cell) const;

    // Cubic metres of a seam under the cell.
    [[nodiscard]] double volume(CellId cell, const Deposit& deposit) const;

private:
    [[nodiscard]] Strata strata(CellId cell, Biome biome) const;
    [[nodiscard]] NoisePoint noise_at(CellId cell, double wavelength) const;

    Climate climate_;
    double planet_radius_;
    std::uint64_t facies_seed_;
    std::uint64_t thickness_seed_;
    std::uint64_t deposit_seed_;
};

} // namespace hog::planet
