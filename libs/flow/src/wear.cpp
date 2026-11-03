#include <hog/flow/wear.hpp>

#include <algorithm>
#include <cmath>

namespace hog::flow {
namespace {

constexpr double year = 365.25 * 86'400.0;

// Each process takes a fully exposed, dry, unfrozen voxel in `lifetime` seconds; damp and frost
// speed it by their gains, since water carries rust and dissolves lime, and frost wedges open
// the pores it filled.
struct Process {
    double lifetime = 0.0; // seconds; 0 for none
    double damp_gain = 0.0;
    double frost_gain = 0.0;
};

Process process_of(globe::Material material) {
    using globe::Material;
    switch (material) {
    case Material::Granite:
        return {200'000 * year, 2.0, 8.0};
    case Material::Basalt:
        return {100'000 * year, 2.0, 6.0};
    case Material::Anorthosite:
        return {200'000 * year, 2.0, 8.0};
    case Material::Sandstone:
        return {20'000 * year, 3.0, 20.0};
    case Material::Limestone:
        return {20'000 * year, 6.0, 10.0};
    case Material::Shale:
        return {5'000 * year, 10.0, 15.0};
    case Material::Iron:
        return {200 * year, 25.0, 0.0};
    default:
        return {};
    }
}

} // namespace

double wearing_rate(globe::Material material, const Exposure& exposure) {
    const Process process = process_of(material);
    if (process.lifetime <= 0.0) {
        return 0.0;
    }
    // Frost only works where there is water to freeze.
    const double speed = 1.0 + process.damp_gain * exposure.damp +
                         process.frost_gain * exposure.freeze * exposure.damp;
    return exposure.open * speed / process.lifetime;
}

std::uint8_t worn(std::uint8_t condition, double rate, double seconds, double chance) {
    const double lost = 255.0 * rate * seconds;
    const double whole = std::floor(lost);
    const double step = whole + (chance < lost - whole ? 1.0 : 0.0);
    return static_cast<std::uint8_t>(std::max(0.0, static_cast<double>(condition) - step));
}

double softness(std::uint64_t key) {
    // splitmix64
    key += 0x9E3779B97F4A7C15ULL;
    key = (key ^ (key >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    key = (key ^ (key >> 27U)) * 0x94D049BB133111EBULL;
    key ^= key >> 31U;
    const double u = static_cast<double>(key >> 11U) / 9007199254740992.0;
    return 0.2 + 2.4 * u * u * u;
}

} // namespace hog::flow
