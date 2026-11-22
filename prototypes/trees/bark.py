"""Bark textures drawn in code, tiling both ways: RGB colour (linear) and A height, 0 deep in
a furrow to 1 on a ridge, which the renderer turns into relief and cavity shading.

u runs around the stem, v along it. Nothing is drawn on straight lines: every pattern is laid
on warped coordinates, carries grain finer than itself, and wears lichen and grime."""

import math

import numpy as np


def _tile_noise(size, cells_u, cells_v, rng, u=None, v=None):
    """Smooth value noise that wraps; at the texture's grid, or at given (u, v) in 0-1."""
    g = rng.random((cells_v, cells_u))
    if u is None:
        v, u = np.mgrid[0:size, 0:size] / size
    y = (v % 1.0) * cells_v
    x = (u % 1.0) * cells_u
    yi, xi = np.floor(y).astype(int), np.floor(x).astype(int)
    fy, fx = y - yi, x - xi
    fy = fy * fy * (3 - 2 * fy)
    fx = fx * fx * (3 - 2 * fx)
    y0, y1 = yi % cells_v, (yi + 1) % cells_v
    x0, x1 = xi % cells_u, (xi + 1) % cells_u
    return ((g[y0, x0] * (1 - fx) + g[y0, x1] * fx) * (1 - fy)
            + (g[y1, x0] * (1 - fx) + g[y1, x1] * fx) * fy)


def _fbm(size, base_u, base_v, rng, octaves=5, gain=0.5, u=None, v=None):
    out = 0.0
    amp, total = 1.0, 0.0
    for k in range(octaves):
        out = out + amp * _tile_noise(size, base_u * 2 ** k, base_v * 2 ** k, rng, u, v)
        total += amp
        amp *= gain
    return out / total


def _warped(size, rng, amount=0.06, base=(3, 3)):
    """Texture coordinates pushed about by noise, still tiling."""
    v, u = np.mgrid[0:size, 0:size] / size
    du = _fbm(size, base[0], base[1], rng, 4) - 0.5
    dv = _fbm(size, base[0], base[1], rng, 4) - 0.5
    return u + du * amount * 2, v + dv * amount * 2


def _cells(u, v, count, rng, stretch=(1.0, 1.0)):
    """Periodic Voronoi over (u, v): nearest and second-nearest distances, nearest index."""
    seeds = rng.random((count, 2))
    d1 = np.full(u.shape, 9.0)
    d2 = np.full(u.shape, 9.0)
    idx = np.zeros(u.shape, int)
    for k, (sx, sy) in enumerate(seeds):
        dx = np.abs((u - sx + 0.5) % 1.0 - 0.5) / stretch[0]
        dy = np.abs((v - sy + 0.5) % 1.0 - 0.5) / stretch[1]
        d = np.hypot(dx, dy)
        nearer = d < d1
        d2 = np.where(nearer, d1, np.minimum(d2, d))
        idx = np.where(nearer, k, idx)
        d1 = np.where(nearer, d, d1)
    return d1, d2, idx


def _weather(rgb, height, rng, size, lichen=0.3, lichen_colour=(0.30, 0.32, 0.24),
             grain=0.15, moss=0.0):
    """Fine grain, lichen crusts on the ridges, moss in the hollows, uneven colour."""
    fine = _fbm(size, 64, 64, rng, 3)
    height = height * (1 - grain * 0.5) + fine * grain * 0.5 * height
    rgb = rgb * (0.88 + 0.24 * fine)[..., None]
    rgb = rgb * (0.85 + 0.3 * _fbm(size, 3, 3, rng, 4))[..., None]
    if lichen:
        spots = _fbm(size, 10, 10, rng, 5)
        crust = np.clip((spots - (0.68 - 0.12 * lichen)) * 10, 0, 1) * np.clip(height * 1.4, 0, 1)
        tone = np.array(lichen_colour) * (0.8 + 0.4 * _fbm(size, 40, 40, rng, 2))[..., None]
        rgb = rgb * (1 - crust[..., None]) + tone * crust[..., None]
        height = height + crust * 0.04
    if moss:
        patch = np.clip((_fbm(size, 5, 5, rng, 5) - (0.62 - 0.15 * moss)) * 6, 0, 1)
        hollow = np.clip(1.2 - height * 1.3, 0, 1)
        m = patch * (0.4 + 0.6 * hollow)
        rgb = rgb * (1 - m[..., None]) + np.array([0.06, 0.09, 0.03]) * m[..., None]
    return rgb, height


def _pack(rgb, height):
    return np.dstack([np.clip(rgb, 0, 1), np.clip(height, 0, 1)]).astype(np.float32)


def furrowed(size=512, colour=(0.10, 0.085, 0.07), deep=(0.035, 0.03, 0.025), seed=1,
             ridges=7, blocks=3.0, diamond=0.0, lichen=0.4, moss=0.4):
    """Thick bark in wandering ridges split by deep furrows, broken across into blocks, as an
    oak's; `diamond` > 0 weaves the furrows into a criss-cross, as a willow's."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.05, (4, 2))
    if diamond:
        a = np.abs(np.sin(math.pi * (u * ridges + v * ridges * 0.5 * diamond)))
        b = np.abs(np.sin(math.pi * (u * ridges - v * ridges * 0.5 * diamond)))
        ridge = np.minimum(a, b) ** 0.7
    else:
        # Furrows of uneven width: some ridges wide, some pinched to nothing.
        width = 0.5 + 0.5 * _fbm(size, ridges, 2, rng, 3, u=u, v=v)
        ridge = np.clip(np.abs(np.sin(math.pi * u * ridges)) * (0.6 + 0.8 * width) - 0.15, 0, 1)
    breaks = _fbm(size, ridges, int(blocks), rng, 3, u=u, v=v)
    crack = np.clip((breaks - 0.6) * 5, 0, 1)
    rough = _fbm(size, 24, 12, rng, 4, u=u, v=v)
    height = ridge ** 0.6 * (1 - 0.75 * crack) * (0.6 + 0.4 * rough)
    rgb = (np.array(deep)[None, None] * (1 - height[..., None])
           + np.array(colour)[None, None] * height[..., None])
    rgb, height = _weather(rgb, height, rng, size, lichen=lichen, moss=moss)
    return _pack(rgb, height)


def birch(size=512, seed=1):
    """Chalk-white birch: dark horizontal lenticels of every length, black fissured patches,
    and torn strips of the papery outer layer peeling to show orange-tan beneath."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.03, (2, 6))
    base = np.array([0.60, 0.59, 0.55])
    rgb = np.tile(base, (size, size, 1)) * (0.88 + 0.15 * _fbm(size, 6, 20, rng, 4))[..., None]
    rgb = rgb * (1 - 0.06 * _fbm(size, 3, 1, rng, 3))[..., None]
    height = np.full((size, size), 0.8)
    for _ in range(170):
        cx, cy = rng.random(), rng.random()
        ln = 0.02 + 0.12 * rng.random() ** 2
        th = 0.002 + 0.005 * rng.random()
        dx = (u - cx + 0.5) % 1 - 0.5
        dy = (v - cy + 0.5) % 1 - 0.5
        box = (np.abs(dx) < ln) & (np.abs(dy) < th * 1.6)
        if not box.any():
            continue
        ragged = th * (0.6 + 0.8 * rng.random())
        m = np.zeros((size, size))
        m[box] = np.clip(1 - (np.abs(dx[box]) / ln) ** 2 - (np.abs(dy[box]) / ragged) ** 2,
                         0, 1) ** 0.4
        dark = np.array([0.04, 0.035, 0.035]) * (0.7 + 0.6 * rng.random())
        rgb = rgb * (1 - m[..., None] * 0.85) + dark * m[..., None] * 0.85
        height = height - m * 0.35
    peel = _fbm(size, 3, 10, rng, 5, u=u, v=v)
    under = np.clip((peel - 0.63) * 14, 0, 1)
    rgb = (rgb * (1 - under[..., None])
           + np.array([0.38, 0.21, 0.10]) * under[..., None]
           * (0.7 + 0.5 * _fbm(size, 20, 40, rng, 3))[..., None])
    edge = np.clip(1 - np.abs(peel - 0.63) * 30, 0, 1) * (_fbm(size, 30, 30, rng, 2) > 0.45)
    height = height - under * 0.25 + edge * 0.25
    rgb = rgb + edge[..., None] * 0.07
    dark = np.clip((_fbm(size, 4, 3, rng, 5, u=u, v=v) - 0.64) * 5, 0, 1)
    rgb = rgb * (1 - dark[..., None] * 0.85)
    height = height * (1 - dark * 0.5) + dark * 0.35 * _fbm(size, 24, 24, rng, 3)
    rgb, height = _weather(rgb, height, rng, size, lichen=0.25,
                           lichen_colour=(0.36, 0.40, 0.30), grain=0.1)
    return _pack(rgb, height)


def plated(size=512, colour=(0.30, 0.15, 0.08), crack=(0.04, 0.03, 0.025), seed=1,
           plates=60, stretch=(1.0, 2.2), flaky=0.0, lichen=0.3):
    """Bark in plates split by ragged fissures: a pine's (tinted by the stem, orange high up,
    grey low down), a larch's or spruce's scales; `flaky` lifts papery flakes off them."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.035, (5, 5))
    d1, d2, idx = _cells(u, v, plates, rng, stretch)
    gap = d2 - d1
    jag = 0.012 + 0.012 * _fbm(size, 30, 30, rng, 3)
    fissure = np.clip(gap / jag, 0, 1)
    tone = rng.random(plates)[idx]
    tilt = rng.random(plates)[idx]
    # Each plate stands a little proud at one end, as scales overlap.
    lift = np.clip(d1 / 0.05, 0, 1) * (tilt - 0.5) * 0.2
    height = fissure ** 0.5 * (0.65 + 0.3 * tone + lift) * (0.8 + 0.2 * _fbm(size, 24, 24, rng, 4))
    rgb = (np.array(crack)[None, None] * (1 - fissure[..., None])
           + np.array(colour)[None, None] * fissure[..., None] * (0.75 + 0.5 * tone[..., None]))
    if flaky:
        f = np.clip((_fbm(size, 12, 18, rng, 4, u=u, v=v) - 0.55) * 8, 0, 1) * fissure
        rgb = rgb * (1 + flaky * 0.45 * f[..., None])
        height = height + flaky * 0.12 * f
    rgb, height = _weather(rgb, height, rng, size, lichen=lichen)
    return _pack(rgb, height)


def smooth(size=512, colour=(0.15, 0.155, 0.15), seed=1):
    """Smooth thin bark, mottled, with lichen spots, dark branch-scar 'eyes' and the odd
    horizontal wrinkle: a beech's."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.04, (3, 3))
    mottle = _fbm(size, 5, 5, rng, 5, u=u, v=v)
    rgb = np.array(colour)[None, None] * (0.75 + 0.45 * mottle)[..., None]
    height = 0.85 + 0.05 * mottle
    wrinkles = np.clip((_fbm(size, 2, 30, rng, 3, u=u, v=v) - 0.66) * 6, 0, 1)
    wrinkles = wrinkles * (_fbm(size, 6, 2, rng, 3) > 0.5)
    height = height - 0.2 * wrinkles
    rgb = rgb * (1 - 0.25 * wrinkles[..., None])
    for _ in range(3):
        cx, cy = rng.random(), rng.random()
        dx = (u - cx + 0.5) % 1 - 0.5
        dy = (v - cy + 0.5) % 1 - 0.5
        r = np.hypot(dx / 0.05, dy / 0.025)
        eye = np.clip(1 - r, 0, 1) ** 0.5
        brow = np.clip(1 - np.abs(r - 1.3) * 3, 0, 1) * (dy < 0)
        rgb = rgb * (1 - 0.6 * eye[..., None]) * (1 - 0.3 * brow[..., None])
        height = height - 0.3 * eye + 0.1 * brow
    rgb, height = _weather(rgb, height, rng, size, lichen=0.25,
                           lichen_colour=(0.20, 0.22, 0.17), grain=0.06)
    return _pack(rgb, height)


def rainbow(size=512, seed=1):
    """Rainbow eucalyptus: bark shed in long ragged strips, each fresh strip bright green,
    aging through olive, blue-grey and purple to orange and maroon. Colours bleed along the
    fibres; torn edges catch the light."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.06, (6, 2))
    d1, d2, idx = _cells(u, v, 34, rng, (1.0, 4.5))
    age = rng.random(34)[idx]
    palette = np.array([[0.14, 0.24, 0.06], [0.11, 0.15, 0.06], [0.08, 0.12, 0.13],
                        [0.12, 0.07, 0.13], [0.30, 0.12, 0.04], [0.22, 0.06, 0.04],
                        [0.15, 0.12, 0.09]])
    fibre = _fbm(size, 10, 50, rng, 4, u=u, v=v)
    stage = np.clip(age + 0.35 * (fibre - 0.5), 0, 0.999) * (len(palette) - 1)
    k = np.minimum(np.floor(stage).astype(int), len(palette) - 2)
    f = (stage - k)[..., None]
    rgb = palette[k] * (1 - f) + palette[k + 1] * f
    rgb = rgb * (0.75 + 0.5 * _fbm(size, 4, 80, rng, 3))[..., None]
    jag = 0.008 + 0.012 * _fbm(size, 40, 20, rng, 3)
    edge = np.clip((d2 - d1) / jag, 0, 1)
    lip = np.clip(1 - np.abs(d2 - d1 - jag) / jag, 0, 1) * (age > 0.4)
    height = 0.7 + 0.15 * age - 0.15 * (1 - edge) + 0.15 * lip + 0.05 * fibre
    rgb = rgb * (0.85 + 0.15 * edge[..., None]) + lip[..., None] * 0.05
    rgb, height = _weather(rgb, height, rng, size, lichen=0.0, grain=0.12)
    return _pack(rgb, height)


def ringed(size=512, colour=(0.22, 0.20, 0.16), seed=1, rings=6):
    """A palm's stem: grey-brown and fibrous, ringed with rough frond scars, uneven in spacing,
    width and depth, cracked here and there."""
    rng = np.random.default_rng(seed)
    u, v = _warped(size, rng, 0.04, (4, 2))
    shift = (_tile_noise(size, 1, rings, rng, u, v) - 0.5) * 0.45
    band = np.abs(np.sin(math.pi * (v * rings + shift)))
    width = 0.08 + 0.12 * _fbm(size, 6, 2, rng, 3)
    scar = np.clip(1 - band / width, 0, 1) * (0.4 + 0.6 * _fbm(size, 14, 4, rng, 3, u=u, v=v))
    fibre = _fbm(size, 50, 6, rng, 4, u=u, v=v)
    cracks = np.clip((_fbm(size, 8, 2, rng, 4, u=u, v=v) - 0.7) * 8, 0, 1) * (1 - scar)
    height = 0.7 + 0.2 * band ** 0.5 - 0.3 * scar + 0.08 * fibre - 0.3 * cracks
    rgb = np.array(colour)[None, None] * (0.7 + 0.45 * fibre)[..., None]
    rgb = rgb * (1 - 0.35 * scar[..., None]) * (1 - 0.5 * cracks[..., None])
    rgb, height = _weather(rgb, height, rng, size, lichen=0.35,
                           lichen_colour=(0.30, 0.30, 0.24), grain=0.1)
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
    for k in ("relief", "twig", "tint_by_stem", "tint_gain", "grime"):
        spec.pop(k, None)
    return KINDS[kind](**spec), tile
