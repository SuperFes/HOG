#include <hog/flow/vent.hpp>

#include <cmath>

namespace hog::flow {

Stage stage_at(const Vent& vent, double clock) {
    if (vent.period <= 0.0) {
        return Stage::Active;
    }
    const double into =
        std::fmod(std::fmod(clock + vent.phase, vent.period) + vent.period, vent.period);
    if (into < vent.eruption) {
        return Stage::Erupting;
    }
    return into < vent.eruption + vent.active ? Stage::Active : Stage::Dormant;
}

double rate_at(const Vent& vent, double clock) {
    switch (stage_at(vent, clock)) {
    case Stage::Erupting:
        return vent.erupting_rate;
    case Stage::Active:
        return vent.active_rate;
    case Stage::Dormant:
        break;
    }
    return 0.0;
}

} // namespace hog::flow
