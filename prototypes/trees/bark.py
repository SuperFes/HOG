"""Bark textures drawn in code, tiling both ways: RGB colour (linear) and A height, 0 deep in
a furrow to 1 on a ridge, which the renderer turns into relief and cavity shading.

u runs around the stem, v along it."""

import math

import numpy as np


def _tile_noise(size, cells_u, cells_v, rng):
    """Smooth value noise that wraps at the texture's edges."""
    g = rng.random((cells_v, cells_u))
    y = np.arange(size) / size * cells_v
    x = np.arange(size) / size * cells_u
    yi, xi = np.floor(y).astype(int), np.floor(x).astype(int)
    fy = y - yi
    fx = x - xi
    fy = (fy * fy * (3 - 2 * fy))[:, None]
    fx = (fx * fx * (3 - 2 * fx))[None, :]
    y0, y1 = yi % cells_v, (yi + 1) % cells_v
    x0, x1 = xi % cells_u, (xi + 1) % cells_u
    a = g[y0][:, x0]
    b = g[y0][:, x1]
    c = g[y1][:, x0]
    d = g[y1][:, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def _fbm(size, base_u, base_v, rng, octaves=5, gain=0.5):
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for k in range(octaves):
        out += amp * _tile_noise(size, base_u * 2 ** k, base_v * 2 ** k, rng)
        total += amp
        amp *= gain
    return out / total


def _cells(size, count, rng, stretch=(1.0, 1.0)):
    """Periodic Voronoi: distance to the nearest and second-nearest seed, and the nearest
    seed's index. `stretch` > 1 elongates cells along that axis (u, v)."""
    seeds = rng.random((count, 2))
    yy, xx = np.mgrid[0:size, 0:size] / size
    d1 = np.full((size, size), 9.0)
    d2 = np.full((size, size), 9.0)
    idx = np.zeros((size, size), int)
    for k, (sx, sy) in enumerate(seeds):
        dx = np.abs(xx - sx)
        dx = np.minimum(dx, 1 - dx) / stretch[0]
        dy = np.abs(yy - sy)
        dy = np.minimum(dy, 1 - dy) / stretch[1]
        d = np.hypot(dx, dy)
        nearer = d < d1
        d2 = np.where(nearer, d1, np.minimum(d2, d))
        idx = np.where(nearer, k, idx)
        d1 = np.where(nearer, d, d1)
    return d1, d2, idx


def _pack(rgb, height):
    return np.dstack([np.clip(rgb, 0, 1), np.clip(height, 0, 1)]).astype(np.float32)


def furrowed(size=512, colour=(0.10, 0.085, 0.07), deep=(0.035, 0.03, 0.025), seed=1,
             ridges=7, blocks=3.0, diamond=0.0):
    """Thick bark in vertical ridges split by deep furrows and broken across into blocks,
    as an oak's; `diamond` > 0 weaves the furrows into a criss-cross, as a willow's."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size] / size
    warp = _fbm(size, 3, 2, rng, 4) - 0.5
    phase = xx * ridges + warp * 1.6
    if diamond:
        cross = np.abs(np.sin(math.pi * (xx * ridges + yy * ridges * 0.5 * diamond + warp)))
        cross2 = np.abs(np.sin(math.pi * (xx * ridges - yy * ridges * 0.5 * diamond + warp)))
        ridge = np.minimum(cross, cross2)
    else:
        ridge = np.abs(np.sin(math.pi * phase))
    breaks = _fbm(size, ridges, int(blocks), rng, 3)
    crack = np.clip((breaks - 0.62) * 6, 0, 1)
    height = ridge ** 0.6 * (1 - 0.8 * crack) * (0.75 + 0.25 * _fbm(size, 16, 16, rng, 4))
    lichen = np.clip((_fbm(size, 4, 4, rng, 4) - 0.62) * 4, 0, 1) * height
    rgb = (np.array(deep)[None, None] * (1 - height[..., None])
           + np.array(colour)[None, None] * height[..., None])
    rgb = rgb * (0.85 + 0.3 * _fbm(size, 32, 64, rng, 3))[..., None]
    rgb = rgb * (1 - lichen[..., None]) + np.array([0.16, 0.17, 0.11]) * lichen[..., None]
    return _pack(rgb, height)


def birch(size=512, seed=1):
    """Chalk-white birch: dark horizontal lenticels, black fissured patches low down, and
    strips of the papery outer layer peeling to show orange-tan beneath."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size] / size
    base = np.array([0.62, 0.61, 0.57])
    rgb = np.tile(base, (size, size, 1)) * (0.9 + 0.12 * _fbm(size, 8, 24, rng, 4))[..., None]
    height = np.full((size, size), 0.8)
    # Lenticels: short dark dashes running around the stem.
    for _ in range(140):
        cx, cy = rng.random(), rng.random()
        ln = 0.03 + 0.09 * rng.random()
        th = 0.003 + 0.004 * rng.random()
        dx = (xx - cx + 0.5) % 1 - 0.5
        dy = (yy - cy + 0.5) % 1 - 0.5
        m = np.clip(1 - (np.abs(dx) / ln) ** 2 - (np.abs(dy) / th) ** 2, 0, 1) ** 0.5
        rgb = rgb * (1 - m[..., None] * 0.8) + np.array([0.05, 0.04, 0.04]) * m[..., None] * 0.8
        height = height - m * 0.35
    # Peeling: strips around the stem where the outer layer has lifted and torn.
    peel = _fbm(size, 3, 10, rng, 4)
    under = np.clip((peel - 0.64) * 12, 0, 1)
    rgb = (rgb * (1 - under[..., None])
           + np.array([0.40, 0.22, 0.10])[None, None] * under[..., None]
           * (0.8 + 0.3 * _fbm(size, 20, 40, rng, 3))[..., None])
    edge = np.clip(1 - np.abs(peel - 0.64) * 40, 0, 1)
    height = height - under * 0.25 + edge * 0.2
    rgb = rgb + edge[..., None] * 0.08
    # Dark rough patches, as old birch bark breaks up.
    dark = np.clip((_fbm(size, 4, 3, rng, 5) - 0.66) * 6, 0, 1)
    rgb = rgb * (1 - dark[..., None] * 0.85)
    height = height * (1 - dark * 0.5) + dark * 0.3 * _fbm(size, 24, 24, rng, 3)
    return _pack(rgb, height)


def plated(size=512, colour=(0.30, 0.15, 0.08), crack=(0.04, 0.03, 0.025), seed=1,
           plates=60, stretch=(1.0, 2.2), flaky=0.0):
    """Bark in plates split by fissures, longer than wide: a pine's (tinted by the stem's own
    colour, orange high up and grey low down), a larch's or spruce's scales; `flaky` lifts
    thin papery flakes off the plates."""
    rng = np.random.default_rng(seed)
    d1, d2, idx = _cells(size, plates, rng, stretch)
    gap = d2 - d1
    fissure = np.clip(gap / 0.02, 0, 1)
    tone = rng.random(plates)[idx]
    height = fissure ** 0.5 * (0.7 + 0.3 * tone) * (0.85 + 0.15 * _fbm(size, 24, 24, rng, 4))
    rgb = (np.array(crack)[None, None] * (1 - fissure[..., None])
           + np.array(colour)[None, None] * fissure[..., None] * (0.8 + 0.4 * tone[..., None]))
    if flaky:
        f = np.clip((_fbm(size, 12, 18, rng, 4) - 0.55) * 8, 0, 1) * fissure
        rgb = rgb * (1 + flaky * 0.5 * f[..., None])
        height = height + flaky * 0.15 * f
    return _pack(rgb, height)


def smooth(size=512, colour=(0.30, 0.31, 0.30), seed=1):
    """Smooth thin bark, mottled, with faint rings around the stem: a beech's."""
    rng = np.random.default_rng(seed)
    yy, _ = np.mgrid[0:size, 0:size] / size
    mottle = _fbm(size, 6, 6, rng, 5)
    rings = np.clip((_fbm(size, 1, 40, rng, 2) - 0.7) * 5, 0, 1)
    rgb = np.array(colour)[None, None] * (0.8 + 0.35 * mottle)[..., None]
    rgb = rgb * (1 - 0.3 * rings[..., None])
    height = 0.85 - 0.25 * rings + 0.05 * mottle
    return _pack(rgb, height)


def rainbow(size=512, seed=1):
    """Rainbow eucalyptus: the bark sheds in patches, each fresh patch bright green, aging
    through blue and purple to orange and maroon, all smooth."""
    rng = np.random.default_rng(seed)
    d1, d2, idx = _cells(size, 46, rng, (1.0, 2.6))
    age = rng.random(46)[idx]
    palette = np.array([[0.20, 0.45, 0.10], [0.12, 0.30, 0.35], [0.20, 0.12, 0.35],
                        [0.55, 0.22, 0.05], [0.40, 0.08, 0.06], [0.30, 0.28, 0.20]])
    stage = age * (len(palette) - 1)
    k = np.minimum(np.floor(stage).astype(int), len(palette) - 2)
    f = (stage - k)[..., None]
    rgb = palette[k] * (1 - f) + palette[k + 1] * f
    streak = _fbm(size, 6, 30, rng, 4)
    rgb = rgb * (0.8 + 0.4 * streak)[..., None]
    edge = np.clip((d2 - d1) / 0.012, 0, 1)
    # Each patch's edge is the lip of the bark it peeled from.
    height = 0.8 + 0.15 * age - 0.3 * (1 - edge)
    rgb = rgb * (0.75 + 0.25 * edge[..., None])
    return _pack(rgb, height)


def ringed(size=512, colour=(0.24, 0.21, 0.17), seed=1, rings=6):
    """A palm's stem: grey-brown, ringed with the scars of fallen fronds."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:size, 0:size] / size
    wob = _tile_noise(size, 4, 1, rng) * 0.06
    band = np.abs(np.sin(math.pi * (yy * rings + wob)))
    scar = np.clip(1 - band / 0.12, 0, 1)
    fibre = _fbm(size, 40, 6, rng, 3)
    height = 0.7 + 0.25 * band ** 0.5 - 0.4 * scar + 0.05 * fibre
    rgb = np.array(colour)[None, None] * (0.75 + 0.4 * fibre)[..., None]
    rgb = rgb * (1 - 0.45 * scar[..., None])
    return _pack(rgb, height)


KINDS = {"furrowed": furrowed, "birch": birch, "plated": plated, "smooth": smooth,
         "rainbow": rainbow, "ringed": ringed}


def make(spec):
    """A bark texture from a species' [bark] table: kind plus that kind's options."""
    spec = dict(spec)
    kind = spec.pop("kind")
    for k in ("colour", "deep", "crack"):
        if k in spec:
            spec[k] = tuple(spec[k])
    if "stretch" in spec:
        spec["stretch"] = tuple(spec["stretch"])
    tile = spec.pop("tile", 0.6)
    spec.pop("relief", None)
    return KINDS[kind](**spec), tile
