#pragma once

#include "cells.hpp"

#include <hog/sim/wildlife.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

// The stages of a wildlife step. Each updates one cell, so cells can run in any order and on any
// thread; everything a cell's animals do to each other happens within it.
namespace hog::sim::detail {

struct LifeContext {
    const WildlifeConfig* config;
    const CellGrid* grid;
    std::span<const Habitat> habitat;
    std::span<const double> areas; // km²
    std::span<const std::vector<float>> peaks;
    Conditions conditions;
    double days;       // the step's length
    double season_day; // days since the March equinox, halfway through the step
    double year;       // years since the epoch, halfway through the step
};

[[nodiscard]] inline bool dormant(const Species& species, double temperature) {
    return species.dormant_below && temperature < *species.dormant_below;
}

[[nodiscard]] inline bool passable(const Species& species, const Habitat& habitat) {
    switch (species.moves) {
    case Moves::Land:
        return !habitat.ocean;
    case Moves::Water:
        return habitat.ocean;
    case Moves::Air:
        return true;
    }
    return false;
}

// How much of a share per day comes to over `days`.
[[nodiscard]] inline double over(double per_day, double days) {
    return 1.0 - std::pow(1.0 - std::clamp(per_day, 0.0, 1.0), days);
}

// The room left for births, 0 to 1: none in a cell it can't live in.
[[nodiscard]] double room(const LifeContext& step, const Species& species, float count,
                          std::uint32_t cell);

// kg per km² of the species' food in the cell, weighted by preference.
[[nodiscard]] double on_offer(const LifeContext& step, const WildlifeFields& fields,
                              std::uint32_t species, std::uint32_t cell);

// Forage grows back or withers towards what the weather allows; carrion rots.
void grow(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell);
// Every species in the cell eats, sharing out what's there when they want more.
void feed(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell);
// Births and deaths; the dead become carrion.
void live(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell);

// How much a species would like to be in a cell, 0 to 1.
[[nodiscard]] double appeal(const LifeContext& step, const WildlifeFields& fields,
                            std::uint32_t species, std::uint32_t cell);

// The share of a cell's animals leaving for each of its neighbours: some wander off at random,
// and some head for where the scent of better ground is stronger.
[[nodiscard]] std::array<float, 4> leaving(const LifeContext& step, std::span<const float> appeal,
                                           std::span<const float> scent, std::uint32_t species,
                                           std::uint32_t cell);

} // namespace hog::sim::detail
