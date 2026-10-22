module shore;

// As hog::terrain's shore grids: texels a side of each layer, finest first.
static const float shore_size = 256.0;
// Where no grid reaches, the sea is taken as too deep for any wave to feel its floor.
public static const float shore_deep = 1000.0;
// Of each layer's half-width, the outer share over which it fades into the next.
static const float shore_fading = 0.2;
// As hog::terrain's shore_highest and shore_reach: depths over ground the sea can't reach are kept
// at the first, and it reaches nothing higher than the second.
public static const float shore_highest = -10.0;
public static const float shore_reach = 5.0;

// How much of the sea's rise over its mean reaches where shore_depth found `depth`: none where the
// grids keep it from, all wherever they let it reach, and so much between along their edges.
public float sea_reached(float depth) {
    return saturate((depth - shore_highest) / (-shore_reach - shore_highest));
}

// As hog::terrain's fetch_headings: fetch is kept for winds blowing towards so many headings, east
// first and on anticlockwise, four to a layer of `fetches`, two of those to each layer of depths.
static const int fetch_headings = 8;
static const float whole_turn = 6.28318530718;

// Where `rest`, a point of sea level from the eye on a planet of `radius`, lies on layers of
// shore_size texels a side, finest first, centred on an anchor at xyz of `anchor` from the eye, in
// the plane `east` and `north` lie along: on each layer, where on its texels, and how far inside it,
// fading out over its edge. The w of `anchor` and `east` are each layer's texels per metre, and
// none at all where there are no layers.
struct ShoreAt {
    float2 centred[2];
    float inside[2];
};

ShoreAt shore_at(float4 anchor, float4 east, float4 north, float radius, float3 rest) {
    ShoreAt at;
    const float3 from_anchor = rest - anchor.xyz;
    const float3 up = cross(east.xyz, north.xyz);
    // Where the line from the planet's centre through it meets the anchor's plane.
    const float2 along = float2(dot(from_anchor, east.xyz), dot(from_anchor, north.xyz)) *
                         (radius / (radius + dot(from_anchor, up)));
    const float per_metre[2] = { anchor.w, east.w };
    for (int layer = 0; layer < 2; ++layer) {
        at.centred[layer] = along * per_metre[layer] / shore_size;
        const float edge = 2.0 * max(abs(at.centred[layer].x), abs(at.centred[layer].y));
        at.inside[layer] = anchor.w > 0.0 ? saturate((1.0 - edge) / shore_fading) : 0.0;
    }
    return at;
}

// Of the sea and still water under `rest`, from `depths` laid as shore_at has them: x metres of the
// sea, y of still water the sea can't reach, and z the level, metres above the sea, still water is
// kept to there. Where no layer lies, the sea is too deep for any wave to feel its floor; over
// land, as high above the water as the grids keep.
public float3 shore_waters(Sampler2DArray depths, float4 anchor, float4 east, float4 north,
                           float radius, float3 rest) {
    const ShoreAt at = shore_at(anchor, east, north, radius, rest);
    float3 waters = float3(shore_deep, shore_highest, 0.0);
    for (int layer = 1; layer >= 0; --layer) {
        if (at.inside[layer] > 0.0) {
            const float4 found =
                depths.SampleLevel(float3(at.centred[layer] + 0.5, float(layer)), 0.0);
            waters = lerp(waters, float3(found.xy, found.z + found.w), at.inside[layer]);
        }
    }
    return waters;
}

// Metres of sea under `rest`, as shore_waters finds it.
public float shore_depth(Sampler2DArray depths, float4 anchor, float4 east, float4 north,
                         float radius, float3 rest) {
    return shore_waters(depths, anchor, east, north, radius, rest).x;
}

// Of a layer of `fetches`, its fetch at `uv` towards the heading `heading`.
float fetch_towards(Sampler2DArray fetches, float2 uv, int layer, int heading) {
    const float4 four = fetches.SampleLevel(float3(uv, float((layer * 2) + (heading / 4))), 0.0);
    return four[heading % 4];
}

// Metres of water upwind of `rest`, laid as shore_at has it, for a wind blowing towards `towards`
// along the anchor's plane, from `fetches` as hog::terrain::shore_fetches works them out.
public float shore_fetch(Sampler2DArray fetches, float4 anchor, float4 east, float4 north,
                         float radius, float3 rest, float3 towards) {
    const ShoreAt at = shore_at(anchor, east, north, radius, rest);
    const float turned = atan2(dot(towards, north.xyz), dot(towards, east.xyz));
    const float heading =
        (turned < 0.0 ? turned + whole_turn : turned) * (float(fetch_headings) / whole_turn);
    const int before = min(int(heading), fetch_headings - 1);
    const int after = (before + 1) % fetch_headings;
    const float between = heading - float(before);
    float fetch = 0.0;
    for (int layer = 1; layer >= 0; --layer) {
        if (at.inside[layer] > 0.0) {
            const float2 uv = at.centred[layer] + 0.5;
            const float found = lerp(fetch_towards(fetches, uv, layer, before),
                                     fetch_towards(fetches, uv, layer, after), between);
            fetch = lerp(fetch, found, at.inside[layer]);
        }
    }
    return fetch;
}
