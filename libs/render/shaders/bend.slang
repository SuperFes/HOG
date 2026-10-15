module bend;

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
    const float turned = atan2(out, from + above);
    const float height = ((2.0 * from * above) + (above * above) + (out * out)) /
                         (length(float2(from + above, out)) + from);

    const float turning = turned * from / to;
    const float half = sin(turning / 2.0);
    const float lifted = (height * cos(turning)) - (2.0 * to * half * half);
    const float spread = (to + height) * sin(turning);
    return (up * (lifted - up_height.w)) + (out > 0.0 ? across * (spread / out) : float3(0.0));
}
