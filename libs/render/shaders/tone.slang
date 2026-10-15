module tone;

// Radiance scaled by exposure, its highlights rolled off rather than clipped.
public float3 tone_mapped(float3 radiance, float exposure) {
    return 1.0 - exp(-radiance * exposure);
}
