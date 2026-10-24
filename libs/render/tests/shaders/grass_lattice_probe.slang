import grass_lattice;

// Two to a case, as bits: a cell's face, i, j and level; then the world's seed, the lattice's
// level and how many levels up to look at most. Two found to a case: the cell's dice as bits, how
// many levels up the blade in it stands for its cells, and where in it the blade stands; then the
// face, i, j and level of the lattice cell standing for it, as bits.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count / 2) {
        return;
    }
    const uint4 at = asuint(asked[2 * id.x]);
    const uint4 settings = asuint(asked[(2 * id.x) + 1]);
    const FaceCell cell = { at.x, at.y, at.z, at.w };
    const uint hash = grass_hash(cell, settings.x);
    found[2 * id.x] =
        float4(asfloat(hash), float(stands_up(cell, settings.x, settings.z)), grass_spot(hash));
    const FaceCell standing = standing_for(cell, settings.y, settings.x);
    found[(2 * id.x) + 1] =
        asfloat(uint4(standing.face, standing.i, standing.j, standing.level));
}
