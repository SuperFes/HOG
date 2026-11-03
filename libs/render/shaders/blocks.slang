import bend;
import gusts;

struct Push {
    float4x4 clip_from_eye;
    // xyz the blocks' origin from the eye; w where the tiles' coordinates start, x.
    float4 origin;
    // xyz up through the eye; w the eye's height above sea level.
    float4 up_height;
    // xyz towards the sun; w the planet's radius.
    float4 sun_radius;
    // x the radius the ground is drawn curving like; y how bright sides away from the sun stay;
    // zw where the tiles' coordinates start, y and z.
    float4 faked_night;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Sampler2DArray tiles;

// Metres a tile repeats over, near and far, and the far tiles the coordinates run over before they
// repeat: render::block_tile, block_far_tile and far_tiles_held.
static const float near_tile = 4.0;
static const float far_tile = 32.0;
static const float held = far_tile * 64.0;
// Metres across the parts of the ground that each wear their far tiles shifted their own way.
static const float part_size = 48.0;
// A plane's share of the texturing below which it's left out, saving its samples.
static const float least_share = 0.02;

struct Vertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    // The tiles at the triangle's three corners, and which of them this corner is in the low two
    // bits of the fourth, with how wet the ground is there in the six over them, as
    // hog::terrain::packed_corner has it; over 47 it is how hot it shines instead.
    [[vk::location(2)]] uint4 tiles;
};

// Soaked, the ground is this much darker: light gets further into it before it comes back.
static const float wet_darkening = 0.45;

struct Surface {
    float4 position : SV_Position;
    float3 normal;
    // The triangle's tiles, the same at each of its corners, and how much of each tile is here.
    nointerpolation uint3 tiles;
    float3 weights;
    float wet;
    // How hot it shines, from 0 to 1.
    float glow;
    // Where the tiles' coordinates lie.
    float3 tiled;
    // Up through the point, from the planet's centre.
    float3 up;
};

[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    const float3 from_eye = vertex.position + push.origin.xyz;
    const float3 drawn =
        bent(from_eye, push.up_height, float2(push.sun_radius.w, push.faked_night.x));
    Surface surface;
    surface.position = mul(push.clip_from_eye, float4(drawn, 1.0));
    surface.normal = vertex.normal;
    surface.tiles = vertex.tiles.xyz;
    const uint corner = vertex.tiles.w & 3u;
    surface.weights = float3(corner == 0 ? 1.0 : 0.0, corner == 1 ? 1.0 : 0.0, corner == 2 ? 1.0 : 0.0);
    // Over 47 the level is how hot it shines instead of how wet it is.
    const uint level = vertex.tiles.w >> 2u;
    surface.wet = level <= 47u ? float(level) / 47.0 : 0.0;
    surface.glow = level > 48u ? float(level - 48u) / 15.0 : 0.0;
    surface.tiled = vertex.position + float3(push.origin.w, push.faked_night.zw);
    surface.up = from_eye + (push.up_height.xyz * (push.sun_radius.w + push.up_height.w));
    return surface;
}

// How much of a side's texturing each of the planet's axes' planes gives: the ones it faces most,
// sharply, and none from those it barely faces.
float3 plane_shares(float3 normal) {
    float3 shares = pow(abs(normal), 4.0);
    shares /= dot(shares, 1.0);
    shares *= step(least_share, shares);
    return shares / dot(shares, 1.0);
}

// The `n`th way a part of the ground shifts its far tiles: anywhere across one.
float2 tile_shift(uint n) {
    uint at = n * 0x9e3779b9u;
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return float2(float(at & 0xffffu), float(at >> 16)) / 65536.0;
}

// Where each plane samples the tiles, and the gradients there, taken while every fragment is still
// running, as sampling skips planes and tiles; and how the far tiles shift here.
struct Tiling {
    float3 shares;
    float2 at[3];
    float2 along_x[3];
    float2 along_y[3];
    float2 shift;
    float2 next;
    float blend;
};

// Each part of the ground wears its far tiles shifted its own way, blending into the next part's,
// so they never line up in rows.
Tiling tiling_at(float3 tiled, float3 normal) {
    Tiling tiling;
    tiling.shares = plane_shares(normal);
    const float2 across[3] = { tiled.yz, tiled.zx, tiled.xy };
    for (uint plane = 0; plane < 3; ++plane) {
        tiling.at[plane] = across[plane];
        tiling.along_x[plane] = ddx(across[plane]);
        tiling.along_y[plane] = ddy(across[plane]);
    }
    const int parts = max(int(held / part_size), 1);
    const float part = 3.0 * (1.0 + periodic_noise(tiled * (float(parts) / held), parts, 9u));
    const float which = floor(part);
    tiling.shift = tile_shift(uint(which));
    tiling.next = tile_shift(uint(which) + 1u);
    tiling.blend = smoothstep(0.4, 0.6, part - which);
    return tiling;
}

// A tile projected along the planet's axes as plane_shares has it, at two scales, each straying
// from the tile's mean in its own way, so together they still average to it.
float3 tile_at(Tiling tiling, uint tile) {
    float3 near = float3(0.0);
    float3 far = float3(0.0);
    for (uint plane = 0; plane < 3; ++plane) {
        if (tiling.shares[plane] == 0.0) {
            continue;
        }
        near += tiling.shares[plane] * tiles.SampleGrad(float3(tiling.at[plane] / near_tile, tile),
                                                        tiling.along_x[plane] / near_tile,
                                                        tiling.along_y[plane] / near_tile).rgb;
        const float2 far_at = tiling.at[plane] / far_tile;
        float3 shifted = tiles.SampleGrad(float3(far_at + tiling.shift, tile),
                                          tiling.along_x[plane] / far_tile,
                                          tiling.along_y[plane] / far_tile).rgb;
        if (tiling.blend > 0.0) {
            shifted = lerp(shifted,
                           tiles.SampleGrad(float3(far_at + tiling.next, tile),
                                            tiling.along_x[plane] / far_tile,
                                            tiling.along_y[plane] / far_tile).rgb,
                           tiling.blend);
        }
        far += tiling.shares[plane] * shifted;
    }
    // The last mip holds the tile's mean.
    const float3 mean = tiles.SampleLevel(float3(0.5, 0.5, tile), 32.0).rgb;
    return near * far / max(mean, 1.0 / 1024.0);
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float night = push.faked_night.y;
    const float3 up = normalize(surface.up);
    const float3 normal = normalize(surface.normal);
    const Tiling tiling = tiling_at(surface.tiled, normal);
    float3 colour = float3(0.0);
    if (surface.tiles.x == surface.tiles.y && surface.tiles.y == surface.tiles.z) {
        colour = tile_at(tiling, surface.tiles.x);
    } else {
        // Each corner's tile fades out towards the others.
        for (uint corner = 0; corner < 3; ++corner) {
            if (surface.weights[corner] > 0.0) {
                colour += surface.weights[corner] * tile_at(tiling, surface.tiles[corner]);
            }
        }
    }
    colour *= 1.0 - (wet_darkening * surface.wet);
    // The sky's light, more on sides that look up at more of it, and some off the ground below,
    // so shapes still show with the sun away.
    const float sky = night * (0.6 + (0.4 * dot(normal, up)));
    // The planet hides the sun once it has set, whichever way a side faces.
    const float risen = smoothstep(-0.02, 0.02, dot(up, push.sun_radius.xyz));
    const float facing = saturate(dot(normal, push.sun_radius.xyz)) * risen;
    float3 lit = colour * (sky + ((1.0 - night) * facing));
    // Hot rock gives its own light, dull red to yellow-white, whatever the sun does; the ground's
    // tile still mottles it so a pool isn't flat.
    const float3 shine = lerp(float3(0.45, 0.04, 0.0), float3(1.0, 0.6, 0.2), surface.glow) *
                         (1.0 + (3.0 * surface.glow * surface.glow));
    lit = lerp(lit, shine * (0.6 + dot(colour, float3(1.0)) * 0.8), saturate(surface.glow * 1.5));
    return float4(lit, 1.0);
}
