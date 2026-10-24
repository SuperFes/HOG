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

// As render::stands_up, and render::thinning_dice as `dice`.
public uint stands_up(FaceCell cell, uint seed, uint most, out uint dice) {
    uint up = 0;
    uint child = 4;
    uint hash = 0;
    bool cast = false;
    while (up < most && cell.level > 0) {
        child = (cell.i & 1u) | ((cell.j & 1u) << 1);
        cell.i >>= 1;
        cell.j >>= 1;
        cell.level -= 1;
        hash = grass_hash(cell, seed);
        cast = true;
        if (picked_child(hash) != child) {
            break;
        }
        child = 4;
        up += 1;
    }
    dice = lattice_hashed((cast ? hash : grass_hash(cell, seed)) + child + 1u);
    return up;
}

// As render::unpicked_child, with render::thinning_dice of it as `dice`.
public FaceCell unpicked_child(FaceCell cell, uint which, uint seed, out uint dice) {
    const uint hash = grass_hash(cell, seed);
    const uint child = which + (which >= picked_child(hash) ? 1u : 0u);
    dice = lattice_hashed(hash + child + 1u);
    const FaceCell finer = { cell.face, (cell.i << 1) | (child & 1u), (cell.j << 1) | (child >> 1),
                             cell.level + 1 };
    return finer;
}

public float2 grass_spot(uint hash) {
    const uint spot = lattice_hashed(hash);
    return float2(float(spot & 0xffffu), float(spot >> 16)) / 65536.0;
}

// As render::clumped: the clumps sparse grass grows in, corners clump_levels up the lattice each
// on or off, the grass growing where they blend to over half, its edges roughened by finer corners.
public static const uint clump_levels = 5;
static const uint clump_wobble_levels = 3;
static const uint clump_salt = 0x517cu;
static const uint wobble_salt = 0xc0bbu;
static const float wobble = 0.2;

float unit_of(uint hash) {
    return float(hash >> 8) / 16777216.0;
}

float mixed(float from, float to, float by) {
    return from + ((to - from) * by);
}

// Where in the cell `levels` up from `blade` the blade stands, 0 to 1 across it.
float2 within(FaceCell blade, float2 spot, uint levels) {
    const uint inside = (1u << levels) - 1u;
    return (float2(float(blade.i & inside), float(blade.j & inside)) + spot) /
           float(inside + 1u);
}

FaceCell corner_of(FaceCell blade, uint levels, uint across, uint up) {
    const FaceCell corner = { blade.face, (blade.i >> levels) + across,
                              (blade.j >> levels) + up, blade.level - levels };
    return corner;
}

float corner_on(FaceCell blade, uint across, uint up, uint seed, float on) {
    return unit_of(grass_hash(corner_of(blade, clump_levels, across, up), seed ^ clump_salt)) < on
               ? 1.0
               : 0.0;
}

float corner_rough(FaceCell blade, uint across, uint up, uint seed) {
    return unit_of(grass_hash(corner_of(blade, clump_wobble_levels, across, up), seed ^ wobble_salt));
}

public float clumped(FaceCell blade, float2 spot, uint seed, float on) {
    const float2 at = within(blade, spot, clump_levels);
    const float corners =
        mixed(mixed(corner_on(blade, 0, 0, seed, on), corner_on(blade, 1, 0, seed, on), at.x),
              mixed(corner_on(blade, 0, 1, seed, on), corner_on(blade, 1, 1, seed, on), at.x),
              at.y);
    const float2 fine = within(blade, spot, clump_wobble_levels);
    const float rough =
        mixed(mixed(corner_rough(blade, 0, 0, seed), corner_rough(blade, 1, 0, seed), fine.x),
              mixed(corner_rough(blade, 0, 1, seed), corner_rough(blade, 1, 1, seed), fine.x),
              fine.y);
    return corners + (2.0 * wobble * (rough - 0.5)) - 0.5;
}
