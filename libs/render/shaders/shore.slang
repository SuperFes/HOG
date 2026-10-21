module shore;

// As hog::terrain's shore grids: texels a side of each layer, finest first.
static const float shore_size = 256.0;
// Where no grid reaches, the sea is taken as too deep for any wave to feel its floor.
public static const float shore_deep = 1000.0;
// Of each layer's half-width, the outer share over which it fades into the next.
static const float shore_fading = 0.2;

// Metres of sea under `rest`, a point of sea level from the eye on a planet of `radius`, from
// `depths`: layers of shore_size texels a side, finest first, centred on an anchor at xyz of
// `anchor` from the eye, in the plane `east` and `north` lie along. The w of `anchor` and `east`
// are each layer's texels per metre, and none at all where there are no depths. Over land, as
// high above the sea as the grids keep.
public float shore_depth(Sampler2DArray depths, float4 anchor, float4 east, float4 north,
                         float radius, float3 rest) {
    if (anchor.w <= 0.0) {
        return shore_deep;
    }
    const float3 from_anchor = rest - anchor.xyz;
    const float3 up = cross(east.xyz, north.xyz);
    // Where the line from the planet's centre through it meets the anchor's plane.
    const float2 along = float2(dot(from_anchor, east.xyz), dot(from_anchor, north.xyz)) *
                         (radius / (radius + dot(from_anchor, up)));
    const float per_metre[2] = { anchor.w, east.w };
    float depth = shore_deep;
    for (int layer = 1; layer >= 0; --layer) {
        const float2 centred = along * per_metre[layer] / shore_size;
        const float edge = 2.0 * max(abs(centred.x), abs(centred.y));
        const float inside = saturate((1.0 - edge) / shore_fading);
        if (inside > 0.0) {
            const float found = depths.SampleLevel(float3(centred + 0.5, float(layer)), 0.0).r;
            depth = lerp(depth, found, inside);
        }
    }
    return depth;
}
