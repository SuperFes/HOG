#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace hog::flow {

// Water at rest over a set of columns once rain has run off them long enough to settle. What
// rain doesn't sink into the ground runs downhill from column to column to where the set lets it
// go: its edge, or the sea. A hollow it runs into fills to the lowest point of its rim if what
// runs in outlasts what its surface gives back to the air, and overflows there; otherwise it fills
// only as far as the two balance. Where water runs through a column it stands in a channel as
// deep as carrying it takes.
class Runoff {
public:
    // The sides of a column: +x, +y, -x, -y.
    static constexpr std::size_t sides = 4;
    static constexpr int none = -1;

    struct Node {
        double ground = 0.0; // metres, the top of what isn't water
        double rain = 0.0;   // metres a year falling on it
        double inflow = 0.0; // cubic metres a year running in from beyond the set
        bool outlet = false; // water reaching it leaves the set
        std::array<int, sides> next = {none, none, none, none};
    };

    struct Law {
        double cell = 1.0;        // metres between columns
        double evaporation = 1.0; // metres a year open water gives back to the air
        double soaks = 0.3;       // share of rain on dry ground that sinks in or is taken up
        double roughness = 0.035; // Manning's coefficient of a channel's bed
        double shallowest = 0.01; // metres; a channel shallower isn't water standing
    };

    Runoff(std::size_t count, Law law) : nodes_(count), law_{law} {}

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] Node& operator[](std::size_t node) { return nodes_[node]; }
    [[nodiscard]] const Node& operator[](std::size_t node) const { return nodes_[node]; }

    // Lets the water find its rest.
    void settle();

    // Where the water stands over a column, metres; its ground where it's dry.
    [[nodiscard]] double surface(std::size_t node) const { return surface_[node]; }
    [[nodiscard]] double depth(std::size_t node) const {
        return surface_[node] - nodes_[node].ground;
    }
    // Cubic metres a year running on out of a column; none out of a hollow that keeps it all.
    [[nodiscard]] double discharge(std::size_t node) const { return discharge_[node]; }
    // Whether the column lies under standing water, a lake or a puddle, rather than a channel.
    [[nodiscard]] bool standing(std::size_t node) const { return standing_[node]; }

private:
    static constexpr int unreached = -2;
    // Columns flooded over their ground to one level, joined side by side, and what runs in.
    struct Hollow {
        std::size_t root = 0; // reached first, it drains out over the rim
        std::vector<std::size_t> columns;
        double inflow = 0.0;
    };

    void flood();
    void find_hollows();
    void gather();
    // Fills the hollow as far as what runs in keeps up with what its surface gives back; what
    // it lets on over its rim.
    double fill(const Hollow& hollow);
    void channels();

    std::vector<Node> nodes_;
    Law law_;
    std::vector<double> surface_;
    std::vector<double> discharge_;
    std::vector<bool> standing_;
    std::vector<double> filled_; // the lowest way out of each column
    std::vector<int> drains_;    // where each column's water goes; none out of the set
    std::vector<std::size_t> order_; // lowest way out first
    std::vector<int> hollow_of_;
    std::vector<Hollow> hollows_;
};

} // namespace hog::flow
