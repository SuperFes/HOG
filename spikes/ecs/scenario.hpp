#pragma once

// The population and components both ECS benchmarks share: 100k entities shaped like a server
// area, mostly creatures, some props, a few players.

#include <malloc.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

namespace spike {

inline constexpr int entity_count = 100'000;
inline constexpr int churn = entity_count / 100;
inline constexpr int lookups = 10'000;
inline constexpr std::uint32_t threads = 8;
inline constexpr double dt = 0.05;
inline constexpr float needs_dt = 0.05F;

struct Position {
    double x, y, z;
};
struct Velocity {
    double x, y, z;
};
struct Cell {
    std::uint64_t key;
};
struct Health {
    float value;
};
struct Hunger {
    float value;
};
struct Creature {};
struct Player {};
struct InCombat {};

enum class Kind : std::uint8_t { Creature, Prop, Player };

// 70% creatures, 25% props, 5% players, interleaved as they would be after a while.
inline Kind kind_of(int index) {
    const int slot = index % 20;
    return slot < 14 ? Kind::Creature : slot < 19 ? Kind::Prop : Kind::Player;
}

inline Position position_of(int index) {
    const auto i = static_cast<double>(index);
    return {std::fmod(i * 7.31, 4'000.0), std::fmod(i * 3.17, 4'000.0), std::fmod(i, 50.0)};
}

inline Velocity velocity_of(int index) {
    const auto i = static_cast<double>(index % 7);
    return {i - 3.0, 3.0 - i, 0.0};
}

// A 64 m grid, standing in for the cube-sphere cell at the spatial index's level.
inline std::uint64_t cell_of(const Position& p) {
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(p.x / 64.0)));
    const auto y = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(p.y / 64.0)));
    return (x << 32U) ^ y;
}

inline std::size_t heap_in_use() {
    const auto info = mallinfo2();
    return info.uordblks + info.hblkhd;
}

using Random = std::mt19937_64;

} // namespace spike
