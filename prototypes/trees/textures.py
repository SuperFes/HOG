"""Foliage textures drawn in code: RGBA, linear colour, alpha as coverage.

Every texture lies along v (0 at the stalk or twig's base, 1 at its tip) and across u."""

import math

import numpy as np


def _grid(w, h):
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    return np.meshgrid(u, v)


def _smooth(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def _noise(shape, rng, scale):
    """Value noise, smooth enough for blotches across a leaf."""
    h, w = shape
    gh, gw = max(int(h / scale), 2) + 2, max(int(w / scale), 2) + 2
    g = rng.random((gh, gw))
    y = np.linspace(0, gh - 2.001, h)
    x = np.linspace(0, gw - 2.001, w)
    yi, xi = np.floor(y).astype(int), np.floor(x).astype(int)
    fy, fx = _smooth(0, 1, y - yi)[:, None], _smooth(0, 1, x - xi)[None, :]
    a = g[yi][:, xi]
    b = g[yi][:, xi + 1]
    c = g[yi + 1][:, xi]
    d = g[yi + 1][:, xi + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def leaf(shape="ovate", size=512, colour=(0.06, 0.13, 0.025), seed=1, serrate=0.0, lobes=0,
         aspect=0.5):
    """One leaf: stalk at v=0, tip at v=1, u across. `aspect` is half-width over length."""
    rng = np.random.default_rng(seed)
    u, v = _grid(size, size)
    x = (u - 0.5) * 2  # -1..1 across
    stalk = 0.08
    t = np.clip((v - stalk) / (1 - stalk), 0, 1)  # along the blade
    if shape == "lanceolate":
        half = np.sin(math.pi * t ** 0.8) ** 1.1 * 0.55
    elif shape == "lobed":
        half = np.sin(math.pi * t ** 0.9) ** 0.8 * (0.62 + 0.38 * np.abs(np.sin(
            math.pi * (lobes + 0.5) * t)) ** 0.7)
    elif shape == "palmate":
        ang = np.arctan2(x * aspect * 2, np.maximum(t, 1e-3))
        half = np.sin(math.pi * t ** 0.7) * (0.7 + 0.3 * np.cos(lobes * ang))
    else:  # ovate, widest below the middle, pointed tip
        half = np.sin(math.pi * t ** 0.65) ** 1.3 * 0.9
    if serrate:
        half = half * (1 - serrate * (0.5 + 0.5 * np.sin(t * 90.0)))
    # Half-width in leaf lengths; the texture spans the widest the leaf can be, `span` each side,
    # so a narrow leaf still fills its card.
    half = half * aspect / 0.9
    span = aspect * 1.1
    reach = np.abs(x) * span
    blade = (reach < half) & (v > stalk)
    edge = _smooth(0.0, 0.012, half - reach)
    alpha = np.where(v > stalk, edge, 0.0)
    alpha = np.maximum(alpha, (np.abs(x) < 0.025) * (v <= stalk + 0.02))
    # Colour: darker toward the edge, a pale midrib and side veins, blotches.
    base = np.array(colour)
    across = reach / np.maximum(half, 1e-3)
    shade = 0.85 + 0.25 * (1 - across) + 0.25 * (_noise(u.shape, rng, 24) - 0.5)
    midrib = np.exp(-(reach / (0.01 * max(span / 0.35, 0.3))) ** 2) * blade
    veins = (np.abs(np.sin((t * 9 - across * 1.5) * math.pi)) < 0.06) * blade * (across < 0.9)
    rgb = base[None, None, :] * shade[..., None]
    rgb = rgb * (1 + 0.8 * midrib[..., None] + 0.25 * veins[..., None])
    rgb = np.where((v <= stalk)[..., None], base * 0.9 + np.array([0.04, 0.02, 0.0]), rgb)
    return np.dstack([np.clip(rgb, 0, 1), np.clip(alpha, 0, 1)])


def needle_spray(size=(256, 1024), colour=(0.025, 0.06, 0.03), seed=1, needle_length=0.35,
                 angle=55.0, density=180, both_sides=True, tufts=False, brush_tip=0.0,
                 width=0.012, side_shoots=0):
    """Needles along a twig running up the middle of the texture: u across, v along.

    `needle_length` is a share of the texture's width; `tufts` gathers them in short-shoot
    rosettes as a larch's; `brush_tip` crowds long needles toward the tip as a pine's."""
    rng = np.random.default_rng(seed)
    w, h = size
    img = np.zeros((h, w, 4))
    yy, xx = np.mgrid[0:h, 0:w]
    base = np.array(colour)

    def stroke(x0, y0, dx, dy, half_w, tint):
        x1, y1 = x0 + dx, y0 + dy
        lo_x, hi_x = int(max(min(x0, x1) - 3, 0)), int(min(max(x0, x1) + 3, w - 1))
        lo_y, hi_y = int(max(min(y0, y1) - 3, 0)), int(min(max(y0, y1) + 3, h - 1))
        if lo_x >= hi_x or lo_y >= hi_y:
            return
        px = xx[lo_y:hi_y, lo_x:hi_x] - x0
        py = yy[lo_y:hi_y, lo_x:hi_x] - y0
        ln2 = dx * dx + dy * dy
        t = np.clip((px * dx + py * dy) / ln2, 0, 1)
        d = np.hypot(px - t * dx, py - t * dy)
        hw = half_w * (1 - 0.7 * t)  # tapering to the tip
        a = _smooth(hw + 0.8, hw - 0.3, d)
        region = img[lo_y:hi_y, lo_x:hi_x]
        # Lighter along each needle's length toward its tip; over what's under it.
        col = tint * (0.8 + 0.4 * t)[..., None]
        region[..., :3] = region[..., :3] * (1 - a[..., None]) + col * a[..., None]
        region[..., 3] = np.maximum(region[..., 3], a)

    cx = w / 2
    # The twig itself.
    stroke(cx, 0, 0, h * 0.98, w * width * 1.2, np.array([0.08, 0.05, 0.03]))
    length = needle_length * w
    if side_shoots:
        # A branchlet: side shoots off the twig, every shoot thick with short needles.
        rng2 = np.random.default_rng(seed + 7)
        shoots = []
        for k in range(side_shoots):
            y0 = (k + 0.5) / side_shoots * h * 0.92
            for sgn in (-1, 1):
                ang = math.radians(48 + rng2.normal() * 6)
                ln = w * 0.46 * (0.6 + 0.4 * math.sin(math.pi * min((y0 / h) * 1.1, 1.0)))
                shoots.append((cx, y0 + rng2.normal() * 4, sgn * math.sin(ang) * ln,
                               math.cos(ang) * ln))
        shoots.append((cx, 0, 0, h * 0.98))
        for x0, y0, dx, dy in shoots:
            stroke(x0, y0, dx, dy, w * width * 0.6, np.array([0.08, 0.05, 0.03]))
        nl = needle_length * w * 0.25
        for x0, y0, dx, dy in shoots:
            ln = math.hypot(dx, dy)
            ux, uy = dx / ln, dy / ln
            for _ in range(int(density * ln / h)):
                t = rng.random()
                px, py = x0 + dx * t, y0 + dy * t
                for sgn in (-1, 1):
                    a = math.radians(55 + rng.normal() * 12) * sgn
                    nx = ux * math.cos(a) - uy * math.sin(a)
                    ny = ux * math.sin(a) + uy * math.cos(a)
                    ll = nl * (0.7 + 0.5 * rng.random())
                    tint = base * (0.7 + 0.6 * rng.random())
                    stroke(px, py, nx * ll, ny * ll, w * width * 0.35, tint)
    elif tufts:
        for _ in range(int(density / 12)):
            y0 = rng.random() * h * 0.95
            for _k in range(14):
                a = rng.random() * 2 * math.pi
                ln = length * (0.4 + 0.3 * rng.random())
                tint = base * (0.8 + 0.4 * rng.random())
                stroke(cx, y0, math.cos(a) * ln, math.sin(a) * ln * 0.6 + ln * 0.3,
                       w * width * 0.6, tint)
    else:
        for _ in range(density):
            y0 = rng.random() ** (1.0 / (1.0 + brush_tip * 2)) * h * 0.97
            side = 1 if (not both_sides or rng.random() < 0.5) else -1
            ang = math.radians(angle + rng.normal() * 10)
            ln = length * (0.75 + 0.35 * rng.random()) * (1 + brush_tip * (y0 / h))
            # Needles lean toward the tip, a little curved.
            dx = side * math.sin(ang) * ln
            dy = math.cos(ang) * ln
            tint = base * (0.75 + 0.5 * rng.random())
            stroke(cx + side * rng.random() * 2, y0, dx, dy, w * width * 0.5, tint)
    img[..., :3] = np.where(img[..., 3:] > 0, img[..., :3], base * 0.7)
    return np.clip(img, 0, 1)


def frond_half(size=(512, 1024), colour=(0.07, 0.12, 0.03), seed=1, leaflets=70,
               angle=60.0, gap=0.35):
    """One side of a pinnate frond: rachis along v at u=0, leaflets running out toward u=1.

    Leaflets are long narrow blades with a fold line, apart by `gap` of their spacing."""
    rng = np.random.default_rng(seed)
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    img = np.zeros((h, w, 4))
    base = np.array(colour)
    spacing = h / leaflets
    for k in range(leaflets):
        y0 = (k + 0.5) * spacing
        along = y0 / h
        # Longest in the frond's middle, short at its foot and tip.
        ln = w * (0.35 + 0.65 * math.sin(math.pi * min(max(along * 1.05, 0.02), 1.0)) ** 0.7)
        ang = math.radians(angle + rng.normal() * 4)
        dx, dy = math.sin(ang) * ln, math.cos(ang) * ln
        half_w = spacing * (1 - gap) * 0.5 / max(math.sin(ang), 0.3)
        px, py = xx, yy - y0
        t = np.clip((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1)
        d = np.hypot(px - t * dx, py - t * dy)
        hw = half_w * np.sin(math.pi * np.clip(t * 0.95 + 0.05, 0, 1)) ** 0.5
        a = _smooth(hw + 0.8, hw - 0.4, d) * (t < 0.999)
        # A pale midrib down each leaflet, darker toward its edges.
        rel = d / np.maximum(hw, 1e-3)
        tint = base * (0.8 + 0.4 * rng.random()) * (1.1 - 0.3 * rel)[..., None]
        tint = tint * (1 + 0.5 * np.exp(-(rel / 0.12) ** 2))[..., None]
        img[..., :3] = img[..., :3] * (1 - a[..., None]) + tint * a[..., None]
        img[..., 3] = np.maximum(img[..., 3], a)
    # The rachis down the near edge.
    rach = _smooth(6, 3, xx)
    img[..., :3] = img[..., :3] * (1 - rach[..., None]) + np.array([0.18, 0.15, 0.06]) * rach[
        ..., None]
    img[..., 3] = np.maximum(img[..., 3], rach)
    img[..., :3] = np.where(img[..., 3:] > 0, img[..., :3], base * 0.7)
    return np.clip(img, 0, 1)


def to_srgb8(img):
    """For saving to look at: linear RGBA to 8-bit sRGB."""
    rgb = np.clip(img[..., :3], 0, 1) ** (1 / 2.2)
    return (np.dstack([rgb, img[..., 3]]) * 255).astype(np.uint8)
