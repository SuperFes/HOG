module mirror;

// Taps round the way looked along, out from it on a golden-angle spiral, so they cover a round
// patch evenly.
static const int mirror_taps = 16;
static const float golden_angle = 2.39996323;
// Texels across a face of the probe's cube, as hog::render::EnvironmentProbe::default_size.
static const float probe_size = 128.0;

// What `around` holds along `way` in its own axes, blurred round it by a Gaussian `spread`
// radians wide: each tap read from the mip whose texels are as wide as the taps lie apart, so no
// texel shows between them. Its alpha is how much of that the probe took.
public float4 blurred(SamplerCube around, float3 way, float spread) {
    const float3 along = normalize(way);
    const float3 helper = abs(along.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 across = normalize(cross(helper, along));
    const float3 up = cross(along, across);
    const float texel = 1.5707963 / probe_size;
    const float apart = 2.0 * spread / sqrt(float(mirror_taps));
    const float level = max(0.0, log2(apart / texel));
    float4 sum = float4(0.0);
    float weights = 0.0;
    for (int tap = 0; tap < mirror_taps; ++tap) {
        // Out to two widths, the Gaussian weighing those further out less.
        const float out = sqrt((float(tap) + 0.5) / float(mirror_taps)) * 2.0;
        const float turned = float(tap) * golden_angle;
        const float3 toward =
            normalize(along + (((across * cos(turned)) + (up * sin(turned))) * (out * spread)));
        const float weight = exp(-0.5 * out * out);
        sum += weight * around.SampleLevel(toward, level);
        weights += weight;
    }
    return sum / weights;
}
