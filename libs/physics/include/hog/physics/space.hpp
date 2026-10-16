#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::physics {

// Triangles that never move, such as a piece of the ground.
struct Mesh {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    std::vector<Vec3> vertices; // metres from the origin
    // Indices of vertices, counter-clockwise seen from the side that collides.
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

enum class BodyId : std::uint32_t {};

struct Hit {
    BodyId body{};
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition at = {};
    DVec3 normal{0.0}; // unit, out of the side hit
};

struct SpaceSettings {
    std::uint32_t bodies = 16'384;
    std::uint32_t body_pairs = 16'384;
    std::uint32_t contacts = 8'192;
};

// A world of bodies in planet-centred coordinates, as precise anywhere as WorldPosition is.
class Space {
public:
    explicit Space(const SpaceSettings& settings = {});
    ~Space();
    Space(Space&&) noexcept;
    Space& operator=(Space&&) noexcept;
    Space(const Space&) = delete;
    Space& operator=(const Space&) = delete;

    [[nodiscard]] Result<BodyId> add(const Mesh& mesh);
    void remove(BodyId body);
    [[nodiscard]] std::size_t size() const noexcept;

    // The first thing a ray from `from` meets within `along`'s length, coming at its front.
    [[nodiscard]] std::optional<Hit> cast_ray(WorldPosition from, DVec3 along) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::physics
