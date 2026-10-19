module season;

// The year as broadleaves keep it, as libs/flora's season.cpp has it: change both together.

// As flora::Leaves.
public struct Leaves {
    public float turned; // 0 green, 1 as deep into autumn's colours as they go
    public float held;   // the share still on the tree
};

// As flora::own_season, given the tree's dice unit(hash + 12).
public float own_season(float dice, float age) {
    return clamp((0.7 * ((2.0 * dice) - 1.0)) + (0.6 * (age - 0.5)), -1.0, 1.0);
}

static const float colouring = 0.6;
static const float dropping = 0.4;
static const float flushing = 0.1;

// `leafing` as flora::Leafing: x turn, y fall, z flush, w spread.
Leaves autumn(float4 leafing, float own, float hours) {
    const float dark = leafing.x + (own * leafing.w) - hours;
    Leaves leaves;
    leaves.turned = saturate(dark / (colouring * leafing.y));
    leaves.held = 1.0 - saturate((dark - (dropping * leafing.y)) / (colouring * leafing.y));
    return leaves;
}

// As flora::leaves_at, the year as Lighting.year keeps it.
public Leaves leaves_at(float4 leafing, float own, float4 year) {
    Leaves leaves;
    leaves.turned = 0.0;
    leaves.held = 1.0;
    if (leafing.x <= 0.0) {
        return leaves;
    }
    if (year.z < 0.5) {
        return autumn(leafing, own, year.x);
    }
    const float shortest = 24.0 - year.y;
    const Leaves winter = autumn(leafing, own, shortest);
    const float range = year.y - shortest;
    if (range <= 0.0) {
        return winter;
    }
    const float into = (year.x - shortest) / range;
    const float out = saturate((into - leafing.z - (0.05 * own)) / flushing);
    leaves.turned = winter.turned * (1.0 - out);
    leaves.held = max(winter.held, out);
    return leaves;
}

// Leaves `turned` from `green` through their kind's first autumn colour to its last.
public float3 autumn_colour(float3 green, float turned, float3 first, float3 last) {
    return turned < 0.5 ? lerp(green, first, turned * 2.0) : lerp(first, last, (turned * 2.0) - 1.0);
}
