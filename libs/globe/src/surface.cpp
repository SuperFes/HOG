#include <hog/globe/maths.hpp>
#include <hog/globe/surface.hpp>

#include <cmath>

namespace hog::globe {

double Surface::holds(double thickness, double gravity) const {
    if (grain == Material::Air) {
        return maths::tan(footing(bed, wetness));
    }
    const double depth =
        static_cast<double>(grain_amount) / static_cast<double>(Voxel::full) * thickness;
    return holding_slope(grain, bed, wetness, depth, gravity);
}

} // namespace hog::globe
