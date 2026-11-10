#pragma once

#include <hog/formation/body.hpp>
#include <hog/globe/planet.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace hog::globe::knobs {

using namespace std::string_view_literals;

// Every knob of a planet by its name in [planet], one place each, for reading it, sending it and
// telling worlds apart. The formation's star_flux isn't one: the system's stars set it.

inline constexpr std::array body_numbers{
    std::pair{"radius"sv, &formation::Body::radius},
    std::pair{"land_share"sv, &formation::Body::land_share},
    std::pair{"plate_speed"sv, &formation::Body::plate_speed},
    std::pair{"boundary_age"sv, &formation::Body::boundary_age},
    std::pair{"crust_thickness"sv, &formation::Body::crust_thickness},
    std::pair{"crust_rise"sv, &formation::Body::crust_rise},
    std::pair{"continent_height"sv, &formation::Body::continent_height},
    std::pair{"ridge_depth"sv, &formation::Body::ridge_depth},
    std::pair{"abyss_depth"sv, &formation::Body::abyss_depth},
    std::pair{"floor_sinking"sv, &formation::Body::floor_sinking},
    std::pair{"smallest_sea"sv, &formation::Body::smallest_sea},
    std::pair{"lid_thickness"sv, &formation::Body::lid_thickness},
    std::pair{"lid_stiffness"sv, &formation::Body::lid_stiffness},
    std::pair{"mantle_density"sv, &formation::Body::mantle_density},
    std::pair{"liquid_density"sv, &formation::Body::liquid_density},
    std::pair{"thickening"sv, &formation::Body::thickening},
    std::pair{"trench_depth"sv, &formation::Body::trench_depth},
    std::pair{"arc_height"sv, &formation::Body::arc_height},
    std::pair{"rift_depth"sv, &formation::Body::rift_depth},
    std::pair{"uplift_share"sv, &formation::Body::uplift_share},
    std::pair{"cover_thickness"sv, &formation::Body::cover_thickness},
    std::pair{"rift_fill"sv, &formation::Body::rift_fill},
    std::pair{"sea_floor_mud"sv, &formation::Body::sea_floor_mud},
    std::pair{"layering"sv, &formation::Body::layering},
    std::pair{"soluble_share"sv, &formation::Body::soluble_share},
    std::pair{"old_folds"sv, &formation::Body::old_folds},
    std::pair{"arc_lava"sv, &formation::Body::arc_lava},
    std::pair{"lava_flood_reach"sv, &formation::Body::lava_flood_reach},
    std::pair{"lava_flood_depth"sv, &formation::Body::lava_flood_depth},
    std::pair{"exhumed_years"sv, &formation::Body::exhumed_years},
    std::pair{"tilt"sv, &formation::Body::tilt},
    std::pair{"day"sv, &formation::Body::day},
    std::pair{"albedo"sv, &formation::Body::albedo},
    std::pair{"ice_albedo"sv, &formation::Body::ice_albedo},
    std::pair{"greenhouse"sv, &formation::Body::greenhouse},
    std::pair{"heat_transport"sv, &formation::Body::heat_transport},
    std::pair{"lapse_rate"sv, &formation::Body::lapse_rate},
    std::pair{"rain_mean"sv, &formation::Body::rain_mean},
    std::pair{"rain_reach"sv, &formation::Body::rain_reach},
    std::pair{"vapour_height"sv, &formation::Body::vapour_height},
    std::pair{"erodibility"sv, &formation::Body::erodibility},
    std::pair{"discharge_power"sv, &formation::Body::discharge_power},
    std::pair{"cover_holding"sv, &formation::Body::cover_holding},
    std::pair{"rock_hardness"sv, &formation::Body::rock_hardness},
    std::pair{"driest_wear"sv, &formation::Body::driest_wear},
    std::pair{"wear_threshold"sv, &formation::Body::wear_threshold},
    std::pair{"hillslope_steepest"sv, &formation::Body::hillslope_steepest},
    std::pair{"hillslope_length"sv, &formation::Body::hillslope_length},
    std::pair{"roughness"sv, &formation::Body::roughness},
    std::pair{"sea_floor_steepest"sv, &formation::Body::sea_floor_steepest},
    std::pair{"freezing"sv, &formation::Body::freezing},
    std::pair{"sea_freezing"sv, &formation::Body::sea_freezing},
    std::pair{"season_swing"sv, &formation::Body::season_swing},
    std::pair{"sea_swing"sv, &formation::Body::sea_swing},
    std::pair{"melt_rate"sv, &formation::Body::melt_rate},
    std::pair{"sublimation"sv, &formation::Body::sublimation},
    std::pair{"gravity"sv, &formation::Body::gravity},
    std::pair{"ice_density"sv, &formation::Body::ice_density},
    std::pair{"ice_strength"sv, &formation::Body::ice_strength},
    std::pair{"ice_cooling"sv, &formation::Body::ice_cooling},
    std::pair{"glaciated_years"sv, &formation::Body::glaciated_years},
    std::pair{"glacial_erosion"sv, &formation::Body::glacial_erosion},
    std::pair{"glacial_erosion_power"sv, &formation::Body::glacial_erosion_power},
    std::pair{"overdeepening"sv, &formation::Body::overdeepening},
    std::pair{"lake_loss"sv, &formation::Body::lake_loss},
    std::pair{"shallowest_lake"sv, &formation::Body::shallowest_lake},
    std::pair{"carrying"sv, &formation::Body::carrying},
    std::pair{"sediment_age"sv, &formation::Body::sediment_age},
    std::pair{"soil_making"sv, &formation::Body::soil_making},
    std::pair{"soil_falloff"sv, &formation::Body::soil_falloff},
    std::pair{"weathering_warmth"sv, &formation::Body::weathering_warmth},
    std::pair{"frost_weathering"sv, &formation::Body::frost_weathering},
    std::pair{"slowest_wear"sv, &formation::Body::slowest_wear},
    std::pair{"soil_creep"sv, &formation::Body::soil_creep},
    std::pair{"loose_weathering"sv, &formation::Body::loose_weathering},
    std::pair{"soil_years"sv, &formation::Body::soil_years},
    std::pair{"ice_left_years"sv, &formation::Body::ice_left_years},
    std::pair{"rotting_rock"sv, &formation::Body::rotting_rock},
    std::pair{"plant_growth"sv, &formation::Body::plant_growth},
    std::pair{"root_share"sv, &formation::Body::root_share},
    std::pair{"humus_share"sv, &formation::Body::humus_share},
    std::pair{"rotting"sv, &formation::Body::rotting},
    std::pair{"litter_rotting"sv, &formation::Body::litter_rotting},
    std::pair{"drowned_rotting"sv, &formation::Body::drowned_rotting},
    std::pair{"peat_burial"sv, &formation::Body::peat_burial},
    std::pair{"topsoil_humus"sv, &formation::Body::topsoil_humus},
    std::pair{"peat_density"sv, &formation::Body::peat_density},
    std::pair{"deflation"sv, &formation::Body::deflation},
    std::pair{"deflation_depth"sv, &formation::Body::deflation_depth},
    std::pair{"glacial_deflation"sv, &formation::Body::glacial_deflation},
    std::pair{"beach_sand"sv, &formation::Body::beach_sand},
    std::pair{"rock_deflation"sv, &formation::Body::rock_deflation},
    std::pair{"sand_share"sv, &formation::Body::sand_share},
    std::pair{"silt_share"sv, &formation::Body::silt_share},
    std::pair{"sand_reach"sv, &formation::Body::sand_reach},
    std::pair{"climb_stop"sv, &formation::Body::climb_stop},
    std::pair{"silt_reach"sv, &formation::Body::silt_reach},
    std::pair{"silt_trapped"sv, &formation::Body::silt_trapped},
    std::pair{"thinnest_drift"sv, &formation::Body::thinnest_drift},
    std::pair{"storm_wind"sv, &formation::Body::storm_wind},
    std::pair{"usual_wind"sv, &formation::Body::usual_wind},
    std::pair{"swell_share"sv, &formation::Body::swell_share},
    std::pair{"shore_years"sv, &formation::Body::shore_years},
    std::pair{"cliff_retreat"sv, &formation::Body::cliff_retreat},
    std::pair{"cliff_hardness"sv, &formation::Body::cliff_hardness},
    std::pair{"till_stones"sv, &formation::Body::till_stones},
    std::pair{"rock_sand"sv, &formation::Body::rock_sand},
    std::pair{"stone_life"sv, &formation::Body::stone_life},
    std::pair{"stirring_waves"sv, &formation::Body::stirring_waves},
    std::pair{"drift_reach"sv, &formation::Body::drift_reach},
    std::pair{"volcanic_reach"sv, &formation::Body::volcanic_reach},
    std::pair{"reef_warmth"sv, &formation::Body::reef_warmth},
    std::pair{"shelf_depth"sv, &formation::Body::shelf_depth},
    std::pair{"reef_sand"sv, &formation::Body::reef_sand},
    std::pair{"shell_sand"sv, &formation::Body::shell_sand},
    std::pair{"muddiest_reef"sv, &formation::Body::muddiest_reef},
};

inline constexpr std::array body_counts{
    std::pair{"plates"sv, &formation::Body::plates},
    std::pair{"continents"sv, &formation::Body::continents},
    std::pair{"lava_floods"sv, &formation::Body::lava_floods},
};

// In [planet.ore_abundance].
inline constexpr std::array ores{
    std::pair{"copper"sv, &formation::Body::OreAbundance::copper},
    std::pair{"gold"sv, &formation::Body::OreAbundance::gold},
    std::pair{"placers"sv, &formation::Body::OreAbundance::placers},
    std::pair{"banded_iron"sv, &formation::Body::OreAbundance::banded_iron},
    std::pair{"laterite"sv, &formation::Body::OreAbundance::laterite},
    std::pair{"bog_iron"sv, &formation::Body::OreAbundance::bog_iron},
    std::pair{"lead_zinc"sv, &formation::Body::OreAbundance::lead_zinc},
    std::pair{"salt"sv, &formation::Body::OreAbundance::salt},
    std::pair{"coal"sv, &formation::Body::OreAbundance::coal},
    std::pair{"oil"sv, &formation::Body::OreAbundance::oil},
    std::pair{"diamonds"sv, &formation::Body::OreAbundance::diamonds},
};

inline constexpr std::array shell{
    std::pair{"voxel"sv, &Planet::voxel},
    std::pair{"floor_depth"sv, &Planet::floor_depth},
    std::pair{"sky_height"sv, &Planet::sky_height},
    std::pair{"heat_flow"sv, &Planet::heat_flow},
    std::pair{"formed_detail"sv, &Planet::formed_detail},
};

// A knob added to the formation's Body must be added above, or a planet would drop it. The seed
// and star_flux are the doubles' two extra; lava_floods sits alone among doubles, padded out.
inline constexpr std::size_t lone_int_padding = sizeof(double) - sizeof(int);
static_assert(sizeof(formation::Body) ==
                  ((body_numbers.size() + 1 + 1 + ores.size()) * sizeof(double)) +
                      (body_counts.size() * sizeof(int)) + lone_int_padding,
              "list every Body knob in planet_knobs.hpp");

} // namespace hog::globe::knobs
