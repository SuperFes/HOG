import waves;

struct Push {
    // x where in the frame's Lighting the sea writes whether it lays the water, bytes, as
    // hog::render::sea_laying_at; y 1 if what's drawn beneath it is copied for it to see through;
    // z where it writes the plane of its surface at the eye, as hog::render::sea_water_at.
    uint4 laying;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// As ocean.slang's.
[[vk::binding(0, 0)]] ConstantBuffer<Waves> waves;
[[vk::binding(1, 0)]] Sampler2DArray shore_depths;
[[vk::binding(2, 0)]] Sampler2DArray shore_fetches;
// The frame's Lighting, as atmosphere.slang has it.
[[vk::binding(3, 0)]] RWByteAddressBuffer lighting;

// Fine enough for every wave drawn to be summed whole.
static const float footprint = 0.01;

// The plane of the sea's surface at the eye, and whether the sea lays the water over what's drawn
// beneath it: where it's seen through, as long as the eye stands over its surface. Otherwise the
// eye is in the water, with no surface between it and what's drawn beneath.
[shader("compute")]
[numthreads(1, 1, 1)]
void laid_main() {
    const float3 up = waves.up_height.xyz;
    const float3 under = -up * waves.up_height.w;
    float fetch;
    const float4 floor = floor_at(waves, shore_depths, shore_fetches, under, fetch);
    const WaveSurface water = water_over(waves, under, footprint, floor, fetch);
    const float3 normal = normalize(water.normal);
    const float above = -dot(under + (up * (dot(water.moved, up) + floor.w)), normal);
    lighting.Store(push.laying.x, asuint(push.laying.y != 0 && above > 0.0 ? 1.0 : 0.0));
    lighting.Store4(push.laying.z, asuint(float4(normal, above)));
}
