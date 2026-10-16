module projection;

// Cosines of the slopes up to which ground is projected by the planet's up, and from which by its
// own normal, blending between.
static const float flat_slope = 0.94;  // about 20 degrees
static const float steep_slope = 0.77; // about 40 degrees
// How near the most squarely faced axis another must be, along the direction projected by, to
// share in it. Away from where axes meet, ground takes a single projection.
static const float plane_band = 0.03;

// Shares, summing to 1, of the projections along the planet's x, y and z axes that ground facing
// `normal`, where the planet's up is `up`, takes its materials from. Projecting flat ground by the
// planet's up gives it a single projection and straight seams; steep ground follows its normal, so
// it isn't stretched.
public float3 plane_shares(float3 up, float3 normal) {
    const float steep = 1.0 - smoothstep(steep_slope, flat_slope, dot(normal, up));
    const float3 facing = abs(normalize(lerp(up, normal, steep)));
    const float most = max(facing.x, max(facing.y, facing.z));
    const float3 shares = saturate((facing - (most - plane_band)) / plane_band);
    return shares / (shares.x + shares.y + shares.z);
}
