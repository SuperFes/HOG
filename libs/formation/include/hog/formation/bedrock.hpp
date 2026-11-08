#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <vector>

namespace hog::formation {

// The rock under everything laid since, by the properties that shape land: a hard basement (granite
// and old crystalline rock under light crust, basalt under the sea floor), layered rock laid over
// it, which may alternate hard and soft beds, partly dissolve and lie folded, and lava over both.
struct Bedrock {
    std::vector<double> cover;    // metres of layered rock over the basement
    std::vector<double> layering; // how strongly its hard and soft beds alternate, 0 to 1
    std::vector<double> soluble;  // share of it that dissolves in water, 0 to 1
    std::vector<double> folding;  // how far from lying flat its beds are folded, 0 to 1
    std::vector<double> lava;     // metres of lava and ash lying over it
};

[[nodiscard]] Bedrock bedrock_on(const Sphere& sphere, const Crust& crust, const Body& body);

// What rock comes to the surface once the land has worn down at its present pace.
enum class Rock : std::uint8_t {
    Lava,     // lava flows and ash
    Layered,  // layered rock lying flat or gently tilted
    Folded,   // layered rock folded upright
    Basement, // the light crust's hard crystalline core
    SeaFloor, // the sea floor's basalt
};

struct Outcrop {
    std::vector<Rock> rock;
    std::vector<double> eroded; // metres worn off the top
    std::vector<double> cover;  // metres of layered rock left
    std::vector<double> lava;   // metres of lava left
};

// Layered rock this folded or more counts as folded at the surface.
inline constexpr double folded = 0.5;

[[nodiscard]] Outcrop outcrop_on(const Sphere& sphere, const Crust& crust, const Bedrock& bedrock,
                                 const Rivers& rivers, const Body& body);

// How well rock stands in steep faces and against wear, 0 to 1: crystalline basement best, lava
// and sea-floor basalt nearly as well, folded beds less, flat beds as well as their hard beds and
// limestone let them.
[[nodiscard]] double rock_strength(Rock rock, double layering, double soluble);

// The strength of the rock wear brings to the surface, before the rivers are known: where land has
// come to balance, it wears down as fast as it's pushed up.
[[nodiscard]] std::vector<double> standing_strength(const Sphere& sphere, const Crust& crust,
                                                    const Bedrock& bedrock, const Body& body);

} // namespace hog::formation
