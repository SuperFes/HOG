#pragma once

#include <hog/globe/accretion.hpp>
#include <hog/globe/globe.hpp>

namespace hog::globe {

// A planet born as far as its birth is built: gathered by accretion, then sorted by weight.
[[nodiscard]] Globe birth(const Accretion& setup);

} // namespace hog::globe
