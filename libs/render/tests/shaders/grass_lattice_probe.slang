import grass_lattice;

// Two to a case, as bits: a cell's face, i, j and level; then the world's seed, the lattice's
// level, how many levels up to look at most, and the chance clump corners are on, as is. Three
// found to a case: the cell's dice as bits, how many levels up the blade in it stands for its
// cells, and where in it the blade stands; then the face, i, j and level of the lattice cell
// standing for it, as bits; then how far into a clump its blade stands, and its thinning dice as
// bits; then its first unpicked child's face, i, j and level, as bits.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count / 4) {
        return;
    }
    const uint4 at = asuint(asked[2 * id.x]);
    const uint4 settings = asuint(asked[(2 * id.x) + 1]);
    const FaceCell cell = { at.x, at.y, at.z, at.w };
    const uint hash = grass_hash(cell, settings.x);
    uint dice;
    found[4 * id.x] = float4(asfloat(hash), float(stands_up(cell, settings.x, settings.z, dice)),
                             grass_spot(hash));
    const FaceCell standing = standing_for(cell, settings.y, settings.x);
    found[(4 * id.x) + 1] =
        asfloat(uint4(standing.face, standing.i, standing.j, standing.level));
    found[(4 * id.x) + 2] =
        float4(clumped(cell, grass_spot(hash), settings.x, asfloat(settings.w)), asfloat(dice), 0.0, 0.0);
    uint child_dice;
    const FaceCell child = unpicked_child(cell, 0, settings.x, child_dice);
    found[(4 * id.x) + 3] = asfloat(uint4(child.face, child.i, child.j, child.level));
}
