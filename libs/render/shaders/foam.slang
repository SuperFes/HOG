// As hog::render::foam_size.
static const int foam_size = 512;

struct Push {
    // xy how many texels the grid moved by, as hog::render::FoamStep's shift; z whether anything
    // is kept.
    int4 shift;
    // x the share of foam left that lasts the step.
    float4 fading;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] Sampler2D kept;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> foam;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    const int2 from = int2(id.xy) + push.shift.xy;
    float left = 0.0;
    if (push.shift.z != 0 && all(from >= 0) && all(from < foam_size)) {
        left = kept.Load(int3(from, 0)).x * push.fading.x;
    }
    foam[id.xy] = left;
}
