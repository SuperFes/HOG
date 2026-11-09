#include <hog/server/ice_climate.hpp>

#include <utility>

namespace hog::server {

flow::Region::IceBalance ice_balance(const globe::Planet& planet, ClimateAt climate) {
    return [planet, climate = std::move(climate)](const globe::Cell& column, double radius) {
        return weather::ice_balance(planet, climate(column), radius - planet.body.radius).net();
    };
}

} // namespace hog::server
