#include <hog/globe/birth.hpp>
#include <hog/globe/differentiation.hpp>
#include <hog/globe/settling.hpp>

namespace hog::globe {

Globe birth(const Accretion& setup) {
    Globe globe = accrete(setup);
    (void)differentiate(globe, {.surface = setup.gravity, .radius = setup.radius});
    return globe;
}

} // namespace hog::globe
