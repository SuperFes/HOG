#include <hog/testing/ports.hpp>

#include <random>

namespace hog::testing {

std::uint16_t random_port() {
    std::random_device random;
    return static_cast<std::uint16_t>(lowest_port + (random() % (highest_port - lowest_port + 1U)));
}

} // namespace hog::testing
