import atmosphere;

// As hog::render::rain_ring_cell, rain_ring_layers, rain_ring_life, rain_tile_size,
// rain_tile_cells and rain_tile_layers.
static const float rain_ring_cell = 0.3;
static const int rain_ring_layers = 4;
static const float rain_ring_life = 0.7;
static const int rain_tile_size = 384;
static const int rain_tile_cells = 8;
static const int rain_tile_layers = 2;
// Of the layers of cells, so many are drawn on each of the tile's.
static const int layers_each = rain_ring_layers / rain_tile_layers;
// A ring spreads as fast as the slowest ripples on water run, m/s; its ripples so many metres
// long, as steep as this as it starts.
static const float ring_speed = 0.3;
static const float ring_length = 0.03;
static const float ring_steepest = 0.3;

struct Push {
    // As hog::render::DrawnWaves' rain: x the chance each cell rings, y seconds.
    float4 rain;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// How steeply the rings slope the water along the tile's rows and columns.
[[vk::binding(0, 0)]] [[vk::image_format("rg32f")]] RWTexture2DArray<float2> tile;

// Three numbers from 0 to 1 that look random, the same for the same `seed`.
float3 random_of(int3 seed) {
    // Jarzynski and Olano's PCG3D.
    uint3 v = uint3(seed) * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return float3(v) / 4294967296.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rings_main(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= rain_tile_size)) {
        return;
    }
    const float chance = push.rain.x;
    const float2 texel = (float2(id.xy) + 0.5) * (float(rain_tile_cells) / rain_tile_size);
    float2 slope = float2(0.0);
    for (int each = 0; each < layers_each; ++each) {
        const int layer = (int(id.z) * layers_each) + each;
        const float2 within = texel + (float2(0.37, 0.71) * float(layer));
        const int2 cell = int2(floor(within));
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const int2 at = cell + int2(x, y);
                // Cells a tile apart are the same cell, so the tile repeats seamlessly.
                const int2 seed = ((at % rain_tile_cells) + rain_tile_cells) % rain_tile_cells;
                // Each cell rings in its own time.
                const float lived =
                    (push.rain.y / rain_ring_life) + random_of(int3(seed, layer - rain_ring_layers)).x;
                const float age = frac(lived);
                const float3 drop =
                    random_of(int3(seed, (int(floor(lived)) * rain_ring_layers) + layer));
                if (drop.z >= chance) {
                    continue;
                }
                const float2 from = (within - (float2(at) + drop.xy)) * rain_ring_cell;
                const float out = length(from);
                const float behind = out - (age * rain_ring_life * ring_speed);
                if (abs(behind) > 2.0 * ring_length) {
                    continue;
                }
                const float fading = (1.0 - age) * (1.0 - age);
                const float across = behind / ring_length;
                const float steepness =
                    -ring_steepest * fading * exp(-across * across) * sin(2.0 * pi * across);
                slope += from * (steepness / max(out, 1e-4));
            }
        }
    }
    tile[id] = slope;
}
