import waves;

struct Push {
    // x where in the frame's Lighting the sea writes whether it lays the water, bytes, as
    // hog::render::sea_laying_at; y 1 if what's drawn beneath it is copied for it to see through;
    // z where it writes the plane of its surface at the eye, as hog::render::sea_water_at.
    uint4 laying;
    // A lake's or river's water the eye may be in, w 1 if any: x metres over the sea its top stands
    // at its lowest, y metres it rises over that at its highest, z when, as a share of the year.
    float4 still;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// As ocean.slang's.
[[vk::binding(0, 0)]] ConstantBuffer<Waves> waves;
[[vk::binding(1, 0)]] Sampler2DArray shore_depths;
[[vk::binding(2, 0)]] Sampler2DArray shore_fetches;
// The frame's Lighting, as atmosphere.slang has it.
[[vk::binding(3, 0)]] RWByteAddressBuffer lighting;

static const float two_pi = 6.28318530717959;

// Fine enough for every wave drawn to be summed whole.
static const float footprint = 0.01;

// The plane of the sea's surface at the eye, and whether the sea lays the water over what's drawn
// beneath it: wherever it's seen through, from over its surface or in the water. Under a lake's
// or river's water, its level surface instead.
[shader("compute")]
[numthreads(1, 1, 1)]
void laid_main() {
    const float3 up = waves.up_height.xyz;
    lighting.Store(push.laying.x, asuint(push.laying.y != 0 ? 1.0 : 0.0));
    if (push.still.w > 0.0) {
        // As still water's drawn, rising with the year.
        const float level =
            push.still.x + (push.still.y * 0.5 * (1.0 + cos(two_pi * (waves.year.x - push.still.z))));
        const float above = waves.up_height.w - level;
        if (above < 0.0) {
            lighting.Store4(push.laying.z, asuint(float4(up, above)));
            return;
        }
    }
    const float3 under = -up * waves.up_height.w;
    float4 reach;
    const float4 floor = floor_at(waves, shore_depths, shore_fetches, under, reach);
    const WaveSurface water = water_over(waves, under, footprint, floor, reach);
    const float3 normal = normalize(water.normal);
    const float above = -dot(under + (up * (dot(water.moved, up) + floor.w)), normal);
    lighting.Store4(push.laying.z, asuint(float4(normal, above)));
}
