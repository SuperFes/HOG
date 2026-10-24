module grass_lattice;

// The fixed lattice grass grows on, as render::grass_hash and its fellows have it: a blade at most
// in each of its cells, each in its own spot in the world, whichever patch of ground draws it.

// A cell of the planet's faces: which face, i and j across it, and its level.
public struct FaceCell {
    public uint face;
    public uint i;
    public uint j;
    public uint level;
};

uint lattice_hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

public uint grass_hash(FaceCell cell, uint seed) {
    const uint where = cell.level | (cell.face << 8);
    return lattice_hashed((cell.i * 0x8da6b343u) ^
                          lattice_hashed((cell.j * 0xd8163841u) ^ lattice_hashed(seed ^ where)));
}

// The child of a cell its dice pick: i in the low bit, j in the high.
uint picked_child(uint hash) {
    return hash >> 30;
}

public FaceCell standing_for(FaceCell cell, uint lattice_level, uint seed) {
    while (cell.level < lattice_level) {
        const uint child = picked_child(grass_hash(cell, seed));
        cell.i = (cell.i << 1) | (child & 1u);
        cell.j = (cell.j << 1) | (child >> 1);
        cell.level += 1;
    }
    return cell;
}

public uint stands_up(FaceCell cell, uint seed, uint most) {
    uint up = 0;
    while (up < most && cell.level > 0) {
        const uint child = (cell.i & 1u) | ((cell.j & 1u) << 1);
        cell.i >>= 1;
        cell.j >>= 1;
        cell.level -= 1;
        if (picked_child(grass_hash(cell, seed)) != child) {
            break;
        }
        up += 1;
    }
    return up;
}

public float2 grass_spot(uint hash) {
    const uint spot = lattice_hashed(hash);
    return float2(float(spot & 0xffffu), float(spot >> 16)) / 65536.0;
}
