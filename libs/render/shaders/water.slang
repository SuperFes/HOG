module water;

import atmosphere;
import bend;

// Seawater, as libs/terrain's sea.hpp and water.hpp have it: change both together.

// Each by red, green and blue light, taken at 610, 550 and 470 nm, per metre: water itself
// (Pope and Fry; Morel), which scatters as much back as on.
static const float3 pure_absorbing = float3(0.25, 0.0565, 0.0106);
static const float3 pure_scattering = float3(0.00122, 0.00191, 0.00376);
// Dissolved organics, per unit absorbed at 440 nm, falling off exponentially into the red.
static const float3 organics_absorbing = float3(0.0781, 0.192, 0.6376);
// Plankton, per mg of chlorophyll to the power (Bricaud; Gordon and Morel).
static const float3 plankton_absorbing = float3(0.0065, 0.0060, 0.045);
static const float3 plankton_absorbing_power = float3(0.85, 0.85, 0.75);
static const float3 plankton_scattering = float3(0.2705, 0.3, 0.3511);
static const float plankton_scattering_power = 0.62;
static const float plankton_backward = 0.01;
// Per gram, brown silt washed off the land and grey rock flour ground fine by glaciers, which
// scatters more, and more of the blue, and absorbs less (Babin).
static const float3 silt_absorbing = float3(0.0051, 0.0107, 0.0287);
static const float3 silt_scattering = float3(0.4793, 0.51, 0.5604);
static const float silt_backward = 0.019;
static const float3 flour_absorbing = float3(0.0019, 0.0040, 0.0108);
static const float3 flour_scattering = float3(0.8832, 1.0, 1.2076);
static const float flour_backward = 0.03;
// Of the light scattered forwards, the share that blurs what's seen through the water out of a
// pixel; the rest only moves it within one.
static const float blurring = 0.2;
// How much more clear water scatters back than water does, brightening the deep clear blue the
// sea's drawn darker than it looks; the light scattered by what water carries outweighs it.
static const float clear_brightening = 16.0;

// What the open sea carries, far from any river, as optics takes it.
public static const float4 open_sea = float4(0.01, 0.05, 0.07, 0.0);

// Of water, by the metre: the share lost of the light falling through it, and of what's seen
// through it, which its scattering also blurs away; and of the light falling on it, what deep
// water of it scatters back out.
public struct Optics {
    public float3 dimming;
    public float3 hiding;
    public float3 colour;
};

// Of light scattered back up out of deep water, the share leaving it, by how much the water
// scatters back against all it takes out of the light (Gordon; Lee).
float3 leaving(float3 backward) {
    const float3 under = (0.0949 * backward) + (0.0794 * backward * backward);
    return pi * 0.52 * under / (1.0 - (1.7 * under));
}

// Of water carrying `carried`: x dissolved organic matter, by what it absorbs at 440 nm, per
// metre; y suspended sediment, grams to the cubic metre; z plankton, by its chlorophyll,
// milligrams to the cubic metre; w the share of the sediment that's rock flour, not silt.
public Optics optics(float4 carried) {
    const float sediment = carried.y;
    const float plankton = max(carried.z, 0.0);
    const float flour = saturate(carried.w);
    const float3 by_plankton = plankton_scattering * pow(plankton, plankton_scattering_power);
    const float3 absorbing = pure_absorbing + (carried.x * organics_absorbing) +
                             (plankton_absorbing * pow(float3(plankton), plankton_absorbing_power)) +
                             (sediment * lerp(silt_absorbing, flour_absorbing, flour));
    const float3 scattering =
        pure_scattering + by_plankton + (sediment * lerp(silt_scattering, flour_scattering, flour));
    const float3 backward =
        (pure_scattering / 2.0) + (plankton_backward * by_plankton) +
        (sediment * lerp(silt_scattering * silt_backward, flour_scattering * flour_backward, flour));
    const float3 brightened = backward + ((clear_brightening - 1.0) * pure_scattering / 2.0);
    Optics water;
    water.dimming = absorbing + backward;
    water.hiding = absorbing + backward + (blurring * (scattering - backward));
    water.colour = leaving(brightened / (absorbing + brightened));
    return water;
}

// Reflected straight back, as water does.
public static const float facing_reflection = 0.02;
// The most glancing a view down through the water is taken as, for the length of its path there.
public static const float least_facing = 0.1;

// Of light passing from air into water, the ratio of their refractive indices.
public static const float into_water = 1.0 / 1.333;

// How far a point `under` metres beyond the surface along `looking` seems to move as waves tilt
// the surface's `normal` from `level`'s: as the line of sight bends through it, against as it bends
// through still water.
public float3 bent_by_waves(float3 looking, float3 normal, float3 level, float under) {
    return (refract(looking, normal, into_water) - refract(looking, level, into_water)) * under;
}

// Schlick's: how much is reflected, more the more glancing the view. From under the sea, its
// surface reflects as much back down.
public float reflected_at(float3 looking, float3 normal) {
    const float facing = abs(dot(looking, normal));
    return facing_reflection + ((1.0 - facing_reflection) * pow(1.0 - facing, 5.0));
}

// How much of the sun's light water facing `normal`, its mean square slope `roughness` too fine to
// draw, glints towards the eye, but for the reflection: a normalised Blinn-Phong lobe as wide as it's
// rough.
public float glint_lobe(float3 looking, float3 normal, float roughness, float3 sun) {
    const float sharpness = (2.0 / roughness) - 2.0;
    const float3 halfway = normalize(sun - looking);
    const float lit = saturate(dot(normal, sun) * 10.0);
    return lit * ((sharpness + 8.0) / (8.0 * pi)) * pow(saturate(dot(normal, halfway)), sharpness);
}

// Whether the sea's own pass lays the water over what's drawn beneath it, seen from above, as
// Lighting's reach.w has it; then what's drawn there is drawn as in air.
public bool sea_lays_water(Lighting lighting) {
    return lighting.reach.w > 0.0;
}

// Whether a point `from_eye` lies in the water, as the plane of the sea's surface at the eye has
// it: true to the waves within a hand of the eye, as the near plane is.
public bool in_water(Lighting lighting, float3 from_eye) {
    return dot(from_eye, lighting.water.xyz) + lighting.water.w < 0.0;
}

// Whether a pixel at `ndc`, the picture's own coordinates, is seen from in the water: where the near
// plane there is under the sea's surface.
public bool seen_from_water(Lighting lighting, float2 ndc) {
    const float4 near = mul(lighting.eye_from_clip, float4(ndc, 1.0, 1.0));
    return in_water(lighting, near.xyz / near.w);
}

// `lit` at a point `depth` metres under the sea (negative above it), `along` metres from an eye
// `eye_height` above sea level, looking down at it `looking_down`, the cosine from straight down:
// dimmed by the water, carrying `carried` as optics takes it, that the light passes down to it and
// back to the eye, and filled in by what that water scatters of `on_water`, the light falling on
// the sea. From above, the way through the water is the depth over how steeply the eye looks
// down; and where the sea's pass lays the water, `laid`, from over it or under it, it's left as
// it is.
public float3 through_water(float3 lit, float3 on_water, float4 carried, float depth,
                            float eye_height, float along, float looking_down, bool laid = false) {
    if (laid || (depth <= 0.0 && eye_height >= 0.0)) {
        return lit;
    }
    float seen_through;
    if (eye_height >= 0.0) {
        seen_through = depth / max(looking_down, least_facing);
    } else if (depth >= 0.0) {
        seen_through = along;
    } else {
        // Of the line up out of the water, the part under its surface.
        seen_through = along * eye_height / (eye_height + depth);
    }
    const Optics water = optics(carried);
    const float3 passed = exp(-(water.dimming * max(depth, 0.0)) - (water.hiding * seen_through));
    return (lit * passed) + (water.colour * on_water * (1.0 - passed));
}

// Of light passing through `water` from a point `deep` metres under its surface, `along` metres
// on, sinking `sinking` metres for each (rising if negative): the share let through, and what the
// water scatters on to the end of the way of `on_water`, the light falling on its surface, which
// is dimmed by the water it passes down through to where it's scattered.
public struct WaterBetween {
    public float3 passed;
    public float3 scattered;
};

public WaterBetween water_between(Optics water, float3 on_water, float deep, float sinking,
                                  float along) {
    // Rising, it's in the water only as far as the surface.
    const float under = sinking < 0.0 ? min(along, deep / -sinking) : along;
    // Of what's scattered, the share lost by the metre on, down to it and on to the end of the way.
    const float3 lost = water.hiding + (water.dimming * sinking);
    WaterBetween between;
    between.passed = exp(-water.hiding * under);
    between.scattered =
        water.colour * on_water * exp(-water.dimming * deep) * water.hiding *
        select(abs(lost) > 1e-4, (1.0 - exp(-lost * under)) / lost, float3(under));
    return between;
}

// As through_water, for a point `from_eye` on the planet of `radius`, under an eye `up_height` as
// bend.slang takes it.
public float3 through_water_at(float3 lit, float3 on_water, float4 carried, float3 from_eye,
                               float4 up_height, float radius, bool laid) {
    const float3 up = normalize(from_eye + (up_height.xyz * (radius + up_height.w)));
    return through_water(lit, on_water, carried, -height_above_sea(from_eye, up_height, radius),
                         up_height.w, length(from_eye), dot(-normalize(from_eye), up), laid);
}
