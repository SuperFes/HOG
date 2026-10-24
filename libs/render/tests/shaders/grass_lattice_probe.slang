import grass_lattice;

// Three to a case, as bits: a cell's face, i, j and level; then the world's seed, the lattice's
// level, how many levels up to look at most, and the chance clump corners are on, as is; then a
// layer's depth into the cell, a blade's place in it, and an unpicked child. Six found to a case:
// the cell's dice as bits, how many levels up the blade in it stands for its cells, and where in it
// the blade stands; then the face, i, j and level of the lattice cell standing for it, as bits;
// then how far into a clump its blade stands, its thinning dice, and the child's dithered share;
// then its first unpicked child's face, i, j and level, as bits; then the face, i, j and level of
// the layer's blade's parent, as bits; then which of its unpicked children, as bits, and its dice.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count / 6) {
        return;
    }
    const uint4 at = asuint(asked[3 * id.x]);
    const uint4 settings = asuint(asked[(3 * id.x) + 1]);
    const uint4 layer = asuint(asked[(3 * id.x) + 2]);
    const FaceCell cell = { at.x, at.y, at.z, at.w };
    const uint seed = settings.x;
    const uint hash = grass_hash(cell, seed);
    found[6 * id.x] = float4(asfloat(hash), float(stands_up(cell, seed, settings.z)), grass_spot(hash));
    const FaceCell standing = standing_for(cell, settings.y, seed);
    found[(6 * id.x) + 1] = asfloat(uint4(standing.face, standing.i, standing.j, standing.level));
    found[(6 * id.x) + 2] = float4(clumped(cell, grass_spot(hash), seed, asfloat(settings.w)),
                                   thinning_dice(cell, seed, settings.z),
                                   dithered(cell, layer.z, seed), 0.0);
    const FaceCell child = unpicked_child(cell, 0, seed);
    found[(6 * id.x) + 3] = asfloat(uint4(child.face, child.i, child.j, child.level));
    const Sowing sowing = sown_in_layer(cell, layer.x, layer.y, seed);
    found[(6 * id.x) + 4] =
        asfloat(uint4(sowing.parent.face, sowing.parent.i, sowing.parent.j, sowing.parent.level));
    found[(6 * id.x) + 5] = float4(asfloat(sowing.which), sowing.dice, 0.0, 0.0);
}
