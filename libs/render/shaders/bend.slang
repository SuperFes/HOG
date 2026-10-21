module bend;

// How high above sea level, on the planet of `radius`, a point given from the eye truly is;
// stable for points near the eye over a large planet.
public float height_above_sea(float3 from_eye, float4 up_height, float radius) {
    const float3 up = up_height.xyz;
    const float along_up = dot(from_eye, up);
    const float out = length(from_eye - (up * along_up));
    const float above = along_up + up_height.w;
    return ((2.0 * radius * above) + (above * above) + (out * out)) /
           (length(float2(radius + above, out)) + radius);
}

// The sine and the arctangent of y over x > 0, for the small angles bending turns through: GPUs'
// own need only be precise to an absolute 2^-11 or so near zero, and NVIDIA's are, which the
// planet's radius then multiplies into metres of ground drawn in rings about the eye.
float small_sin(float x) {
    if (abs(x) >= 0.5) {
        return sin(x);
    }
    const float x2 = x * x;
    float series = 1.0 - (x2 / 72.0);
    series = 1.0 - ((x2 / 42.0) * series);
    series = 1.0 - ((x2 / 20.0) * series);
    return x * (1.0 - ((x2 / 6.0) * series));
}

float small_atan(float y, float x) {
    if (x <= 0.0 || abs(y) >= 0.25 * x) {
        return atan2(y, x);
    }
    const float t = y / x;
    const float t2 = t * t;
    float series = 0.0;
    for (int n = 13; n >= 3; n -= 2) {
        series = (1.0 / float(n)) - (t2 * series);
    }
    return t * (1.0 - (t2 * series));
}

// Bends a point given from the eye as hog::terrain::bent does: onto a sphere of radius radii.y
// touching the planet, of radius radii.x, under the eye. up_height is the unit up through the
// eye, and the eye's height above sea level.
public float3 bent(float3 from_eye, float4 up_height, float2 radii) {
    const float from = radii.x;
    const float to = radii.y;
    if (from == to) {
        return from_eye;
    }
    const float3 up = up_height.xyz;
    const float along_up = dot(from_eye, up);
    const float3 across = from_eye - (up * along_up);
    const float out = length(across);
    const float above = along_up + up_height.w;
    const float turned = small_atan(out, from + above);
    const float height = height_above_sea(from_eye, up_height, from);

    const float turning = turned * from / to;
    const float half = small_sin(turning / 2.0);
    const float versed = 2.0 * half * half; // 1 - cos(turning), without cancelling
    const float lifted = (height * (1.0 - versed)) - (to * versed);
    const float spread = (to + height) * small_sin(turning);
    return (up * (lifted - up_height.w)) + (out > 0.0 ? across * (spread / out) : float3(0.0));
}
