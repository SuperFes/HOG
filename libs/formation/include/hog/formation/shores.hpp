#pragma once

#include <hog/formation/bedrock.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/sphere.hpp>

#include <vector>

namespace hog::formation {

// What the sea's waves make of its edge since it came to stand at its level. They sort what
// reaches the coast (the rivers' load, rock worn off the coast itself, shells and coral grown in
// warm shallow water) and carry it along the shore: exposed coasts keep sand and stones and wash
// the mud out to sea, sheltered ones keep the mud, and coasts nothing reaches stay bare rock.
// Held at the points either side of the waterline, none elsewhere.
struct Shores {
    std::vector<double> waves; // metres: the significant height of storms' waves on the coast
    std::vector<double> usual; // and of an ordinary day's, which build the beach back up
    std::vector<double> beach; // cubic metres of beach a metre of coast holds
    // What the beach is made of, shares summing to 1 wherever there's any.
    std::vector<double> quartz;    // sand worn off the light crust's rock
    std::vector<double> volcanic;  // sand worn off lava and basalt, dark and heavy
    std::vector<double> carbonate; // shells and coral broken up, pale
    std::vector<double> shingle;   // stones the surf has rounded
    std::vector<double> mud;       // cubic metres of mud a metre of sheltered coast holds
    std::vector<double> reef;      // 0 to 1: how likely coral reefs grow off the coast
    std::vector<double> retreat;   // metres a year the waves wear the land's edge back
};

// The significant height of waves a storm's wind raises over `fetch` metres of open water, up to
// the height of a fully grown sea (JONSWAP growth, Pierson-Moskowitz limit).
[[nodiscard]] double wave_height(double fetch, const Body& body);
// Likewise for a wind blowing `wind` metres a second.
[[nodiscard]] double wave_height(double fetch, double wind, const Body& body);
// Seconds between the crests of waves `height` metres high, as steep as a fully grown sea's. Swell
// keeps the period of the storm's sea it came from as its height dies away.
[[nodiscard]] double wave_period(double height, const Body& body);

[[nodiscard]] Shores shores_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers,
                               const Bedrock& bedrock, const Outcrop& outcrop, const Ice& ice,
                               const Sediment& sediment, const Body& body);

} // namespace hog::formation
