module lush;

import gusts;

// How thick the grass grows, from patch to patch: a field fixed to the ground, lying `anchor` from
// the eye as Lighting's ground has it, repeating over the gusts' period. Lusher patches tens of
// metres across, sparser ones between, gathered unevenly into broader swathes.

// The most and least times the biome's own density a patch grows.
public static const float thickest = 3.0;
public static const float thinnest = 0.7;
// How much taller the grass stands where it grows thickest.
public static const float taller = 0.35;

// Times the biome's density grass grows at a point given from the eye, thinnest to thickest.
public float thickness(float3 from_eye, float3 anchor) {
    // Lattices of uneven sizes, about 89, 34 and 14 m, kept apart from the gusts' by their
    // octaves; the broadest pushing the others about, so their patches never fall into rows.
    const float3 at = (from_eye + anchor) / period;
    const float broad = periodic_noise(at * 23.0, 23, 6u);
    const float3 pushed = float3(0.9, -0.6, 0.4) * broad;
    const float field = (0.5 * broad) + (0.35 * periodic_noise((at * 60.0) + pushed, 60, 7u)) +
                        (0.2 * periodic_noise((at * 149.0) - pushed, 149, 8u));
    return lerp(thinnest, thickest, smoothstep(-0.35, 0.45, field));
}

// Times its height grass stands where it grows `thick`.
public float thick_height(float thick) {
    return 1.0 + (taller * saturate((thick - 1.0) / (thickest - 1.0)));
}
