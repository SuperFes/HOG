#include <hog/client/tides.hpp>

#include <cstddef>

namespace hog::client {

std::vector<waves::Pull> tide_pulls(const planet::Ephemeris& ephemeris, GameTime time) {
    const planet::System& system = ephemeris.system();
    std::vector<waves::Pull> pulls;
    pulls.reserve(system.stars.size() + system.moons.size());
    for (std::size_t star = 0; star < system.stars.size(); ++star) {
        pulls.push_back(
            {.toward = ephemeris.star(star, time), .height = ephemeris.star_tide(star, time)});
    }
    for (std::size_t moon = 0; moon < system.moons.size(); ++moon) {
        pulls.push_back(
            {.toward = ephemeris.moon(moon, time), .height = ephemeris.moon_tide(moon, time)});
    }
    return pulls;
}

} // namespace hog::client
