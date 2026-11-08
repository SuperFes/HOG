#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// How two plates meet where they touch.
enum class Meeting : std::uint8_t {
    None,       // no boundary near
    Collision,  // light crust against light crust: it piles up into broad mountains
    Subduction, // sea floor sinking under light crust: a trench, and volcanoes behind it
    IslandArc,  // sea floor sinking under sea floor: a trench and a chain of volcanic islands
    Ridge,      // sea floor pulled apart: new floor made, standing high
    Rift,       // light crust pulled apart: a sagging valley
    Transform,  // sliding past: nothing built
};

// One stretch of boundary between two plates on their layout, and what's been happening along it.
struct Boundary {
    DVec3 where{};
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    Meeting meeting = Meeting::None;
    std::uint16_t over = 0xFFFF; // the plate riding over, where one sinks
    double closing = 0.0;        // radians a million years; under 0 pulling apart
    double width = 0.0;          // radians to its belt's edge
    double thickened = 0.0;      // metres the crust has thickened at its middle
};

// The plates as they are now and how they've been moving: laid out on a coarse sphere, each
// spinning about its own pole. Not a simulation; their history is what their motions imply.
class Plates {
public:
    Plates(const Body& body, const Sphere& layout);

    // How the plates meet, decided along the layout's boundaries at the plates' own scale.
    [[nodiscard]] std::span<const Boundary> boundaries() const noexcept { return boundaries_; }

    [[nodiscard]] const Sphere& layout() const noexcept { return *layout_; }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(spins_.size()); }
    // The plate under each point of the layout.
    [[nodiscard]] std::uint16_t plate(std::uint32_t point) const { return plates_[point]; }
    // Whether light crust lies at each point of the layout.
    [[nodiscard]] bool light(std::uint32_t point) const { return light_[point] != 0; }
    // Radians a million years about the plate's pole, as a vector along it.
    [[nodiscard]] const DVec3& spin(std::uint16_t plate) const { return spins_[plate]; }
    // Radians a million years, along the surface at `where`.
    [[nodiscard]] DVec3 velocity(std::uint16_t plate, const DVec3& where) const {
        return glm::cross(spins_[plate], where);
    }
    // Millions of years the boundary between two plates has been at work.
    [[nodiscard]] double age(std::uint16_t a, std::uint16_t b) const;
    // Of two sea floors meeting, whether `a` rides over `b` rather than sinking under it.
    [[nodiscard]] bool rides_over(std::uint16_t a, std::uint16_t b) const;

private:
    const Sphere* layout_;
    std::uint64_t seed_ = 0;
    double boundary_age_ = 0.0;
    std::vector<std::uint16_t> plates_;
    std::vector<std::uint8_t> light_;
    std::vector<DVec3> spins_;
    std::vector<Boundary> boundaries_;
};

// What the plates leave at each point of a sphere, any finer than their layout: the ground's
// starting height and how fast rock is pushed up, which the stages after read.
struct Crust {
    std::vector<std::uint16_t> plate;
    std::vector<double> light;     // share of light crust, 0 to 1
    std::vector<double> base;      // metres, how the crust stands before anything wears it
    std::vector<double> uplift;    // metres a year rock is pushed up, at Earth's scale
    std::vector<Meeting> meeting;  // the nearest boundary's kind
    std::vector<double> boundary;  // radians to the nearest boundary
    std::vector<double> floor_age; // millions of years since the sea floor was made
    std::vector<std::uint8_t> sea; // 1 under open sea; basins under the level elsewhere are land
};

[[nodiscard]] Crust crust_on(const Sphere& sphere, const Plates& plates, const Body& body);

} // namespace hog::formation
