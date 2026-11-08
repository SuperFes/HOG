#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// Standing water in a hollow of the land, as deep as what flows in can keep against what the air
// takes off its surface: brim-full and spilling into a river when it can, lower and closed when it
// can't, a salt flat when nothing stays.
struct Lake {
    double level = 0.0;       // metres, the water's surface
    double area = 0.0;        // square metres covered, at Earth's scale
    double inflow = 0.0;      // cubic metres a year reaching it
    double loss = 0.0;        // cubic metres a year the air takes off it
    bool open = false;        // brim-full, spilling on downstream
    std::uint32_t outlet = 0; // where its water would leave the hollow
};

// A hollow, wet or dry: everything that runs into it gathers at its outlet.
struct Basin {
    std::uint32_t outlet = 0;
    std::uint32_t lake = 0xFFFF'FFFFU; // the lake standing in it, if any
    double room = 0.0;                 // cubic metres of water it holds
};

struct Lakes {
    static constexpr std::uint32_t none = 0xFFFF'FFFFU;
    std::vector<double> depth;           // metres of standing water over the ground; 0 where none
    std::vector<std::uint32_t> lake;     // which lake covers each point, or none
    std::vector<std::uint32_t> basin;    // which hollow each point lies in, or none
    std::vector<std::uint8_t> salt_flat; // 1 where a hollow dries out
    std::vector<double> discharge;       // the rivers' water after lakes have taken theirs
    std::vector<Lake> lakes;
    std::vector<Basin> basins;
    double to_air = 0.0; // cubic metres a year lakes and salt flats give the air
};

// Metres a year open water loses to the air, more as it warms, little once it freezes over.
[[nodiscard]] double open_water_loss(double celsius, const Body& body);

// How far water fills a hollow: of its points sorted by ground, lowest first, how many it covers
// and where its surface stands, when what the air takes off the covered points matches `inflow`.
// `loss` is cubic metres a year each point would lose under water. Full is `spill`.
struct Filling {
    std::size_t covered = 0;
    double level = 0.0;
};
[[nodiscard]] Filling fill_hollow(std::span<const double> ground, std::span<const double> loss,
                                  double inflow, double spill);

[[nodiscard]] Lakes lakes_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                             const Body& body);

} // namespace hog::formation
