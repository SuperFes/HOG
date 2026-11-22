"""A stand-in landscape for judging forests from far off: 24 km of hills on a height grid,
fields saying how dense the forest is and which kinds grow where, and trees placed one per
cell of a jittered grid by a hash, so any tree can be found again from its cell alone (on
the GPU too, by the same hash)."""

import numpy as np

EXTENT = 24000.0  # metres a side
ORIGIN = np.array([-12000.0, -2000.0])  # world (x, z) of the grid's corner
GRID = 2048
CELL = 7.0  # metres between trees in a full stand
KINDS = 4
EARTH_RADIUS = 6371000.0

HASH_GLSL = """
uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
uint cell_hash(ivec2 c, uint k) {
    return pcg(uint(c.x) + pcg(uint(c.y) + pcg(k)));
}
float unit_hash(ivec2 c, uint k) { return float(cell_hash(c, k) >> 8u) / 16777216.0; }
"""


def _pcg(v):
    s = v * np.uint32(747796405) + np.uint32(2891336453)
    w = ((s >> ((s >> np.uint32(28)) + np.uint32(4))) ^ s) * np.uint32(277803737)
    return (w >> np.uint32(22)) ^ w


def unit_hash(ci, cj, k):
    """Per cell, a number in [0, 1) for draw `k`: the same as the GLSL unit_hash."""
    with np.errstate(over="ignore"):
        h = _pcg(ci.astype(np.uint32) + _pcg(cj.astype(np.uint32) + _pcg(np.uint32(k))))
    return (h >> np.uint32(8)).astype(np.float64) / 16777216.0


def _value_noise(n, freq, rng):
    """Smooth noise on an n x n grid with `freq` lattice cells across it, in [-1, 1]."""
    lat = rng.uniform(-1, 1, (freq + 2, freq + 2))
    t = np.linspace(0, freq, n, endpoint=False)
    i = t.astype(int)
    f = t - i
    f = f * f * (3 - 2 * f)
    a = lat[np.ix_(i, i)] * (1 - f)[None, :] + lat[np.ix_(i, i + 1)] * f[None, :]
    b = lat[np.ix_(i + 1, i)] * (1 - f)[None, :] + lat[np.ix_(i + 1, i + 1)] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def fbm(n, base, octaves, rng, ridged=False):
    out = np.zeros((n, n))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        v = _value_noise(n, base * 2 ** o, rng)
        if ridged:
            v = 1 - 2 * np.abs(v)
        out += v * amp
        total += amp
        amp *= 0.5
    return out / total


class Land:
    """Rows run along z, columns along x."""

    def __init__(self, seed=3):
        rng = np.random.default_rng(seed)
        n = GRID
        x = ORIGIN[0] + (np.arange(n) + 0.5) * EXTENT / n
        z = ORIGIN[1] + (np.arange(n) + 0.5) * EXTENT / n
        X, Z = np.meshgrid(x, z)
        ridges = fbm(n, 6, 7, rng, ridged=True)
        rolling = fbm(n, 3, 6, rng)
        # Hills rising with distance, so far slopes face the eye instead of lying flat.
        rise = np.clip(Z / 14000.0, 0, 1) ** 1.5 * 520.0
        h = 140.0 + rolling * 160.0 + ridges * 260.0 * (0.35 + 0.65 * np.clip(Z / 9000, 0, 1))
        h += rise
        # A summit to look out from, over forested valleys.
        h += 520.0 * np.exp(-(X ** 2 + Z ** 2) / (2 * 600.0 ** 2))
        self.height = h.astype(np.float32)
        gz, gx = np.gradient(self.height, EXTENT / n)
        slope = np.degrees(np.arctan(np.hypot(gx, gz)))
        patches = fbm(n, 10, 6, rng)
        cover = np.clip((patches + 0.12) * 3.5, 0, 1)
        treeline = np.clip((1250.0 - h) / 150.0, 0, 1)
        steep = np.clip((40.0 - slope) / 8.0, 0, 1)
        meadow = np.clip((np.hypot(X, Z) - 250.0) / 80.0, 0, 1)
        # Pasture down the summit's north face, so the eye sees over the trees below.
        pasture = np.clip((np.abs(X) - 0.55 * Z) / 60.0, 0, 1) + np.clip((Z - 1400.0) / 80.0, 0, 1)
        meadow *= np.clip(pasture + (Z < 0), 0, 1)
        self.density = (cover * treeline * steep * meadow).astype(np.float32)
        # Kind shares: spruce up high, birch where the forest is thin, oak and beech below.
        k = np.stack([fbm(n, 5, 4, rng) for _ in range(KINDS)], -1)
        up = np.clip((h - 450.0) / 300.0, 0, 1)
        k[..., 2] += up * 1.5 - 0.3
        k[..., 1] += (1 - cover) * 0.8 - 0.2
        e = np.exp(k * 4.0)
        self.kinds = (e / e.sum(-1, keepdims=True)).astype(np.float32)

    def sample(self, field, x, z):
        """Bilinear lookup of a grid field at world (x, z)."""
        n = GRID
        u = (np.asarray(x) - ORIGIN[0]) / EXTENT * n - 0.5
        v = (np.asarray(z) - ORIGIN[1]) / EXTENT * n - 0.5
        i0 = np.clip(np.floor(u).astype(int), 0, n - 2)
        j0 = np.clip(np.floor(v).astype(int), 0, n - 2)
        fu = np.clip(u - i0, 0, 1)
        fv = np.clip(v - j0, 0, 1)
        if field.ndim == 3:
            fu, fv = fu[..., None], fv[..., None]
        a = field[j0, i0] * (1 - fu) + field[j0, i0 + 1] * fu
        b = field[j0 + 1, i0] * (1 - fu) + field[j0 + 1, i0 + 1] * fu
        return a * (1 - fv) + b * fv

    def trees(self, keep=None):
        """Every tree, as arrays: cell (i, j), position (x, y, z), kind, scale, yaw, rank.
        `keep(x, z)` -> bool filters cells before the rest is worked out."""
        nc = int(EXTENT / CELL)
        ci, cj = np.meshgrid(np.arange(nc), np.arange(nc))
        ci, cj = ci.ravel(), cj.ravel()
        x = ORIGIN[0] + (ci + 0.15 + 0.7 * unit_hash(ci, cj, 1)) * CELL
        z = ORIGIN[1] + (cj + 0.15 + 0.7 * unit_hash(ci, cj, 2)) * CELL
        live = unit_hash(ci, cj, 0) < self.sample(self.density, x, z)
        if keep is not None:
            live &= keep(x, z)
        ci, cj, x, z = ci[live], cj[live], x[live], z[live]
        shares = np.cumsum(self.sample(self.kinds, x, z), -1)
        kind = (unit_hash(ci, cj, 3)[:, None] > shares[:, :-1]).sum(-1)
        return dict(cell=np.stack([ci, cj], 1), pos=np.stack(
            [x, self.sample(self.height, x, z), z], 1), kind=kind,
            scale=0.8 + 0.4 * unit_hash(ci, cj, 4), yaw=unit_hash(ci, cj, 5) * 2 * np.pi,
            rank=unit_hash(ci, cj, 6))
