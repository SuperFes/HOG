#pragma once

#include <hog/formation/bedrock.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/soil.hpp>
#include <hog/formation/sphere.hpp>

#include <vector>

namespace hog::formation {

// How likely each kind of ore is at a point, 0 to 1: where the process making it came together, for
// the bodies of ore themselves, far smaller, to be placed finer.
struct Ores {
    std::vector<double> copper;      // with gold, in the roots of volcanoes behind trenches
    std::vector<double> gold;        // in veins through folded belts
    std::vector<double> placers;     // gold and tin grains dropped in river gravel downstream
    std::vector<double> banded_iron; // laid in ancient seas on the oldest light crust
    std::vector<double> laterite;    // iron and aluminium left in long, hot, wet weathering
    std::vector<double> bog_iron;    // iron gathered in waterlogged peat
    std::vector<double> lead_zinc;   // laid in dissolving layered rock by water moving through it
    std::vector<double> salt;        // left by water drying out, now or long ago
    std::vector<double> coal;        // old swamps' peat buried under layered rock
    std::vector<double> oil;         // cooked out of deep layered rock and caught in its folds
    std::vector<double> diamonds;    // brought up from deep under the oldest light crust
};

[[nodiscard]] Ores ores_on(const Sphere& sphere, const Crust& crust, const Bedrock& bedrock,
                           const Outcrop& outcrop, const Climate& climate, const Rivers& rivers,
                           const Lakes& lakes, const Sediment& sediment, const Soil& soil,
                           const Body& body);

} // namespace hog::formation
