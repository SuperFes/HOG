module tone;

// Radiance scaled by exposure, its highlights rolled off rather than clipped.
public float3 tone_mapped(float3 radiance, float exposure) {
    return 1.0 - exp(-radiance * exposure);
}

// Past this, tone mapped light is too bright to tell apart.
static const float brightest_untoned = 0.999;

// The radiance tone_mapped took to `toned`, as far as it can be told.
public float3 untoned(float3 toned, float exposure) {
    return -log(1.0 - min(toned, brightest_untoned)) / exposure;
}
