"""Foliage meshes on a skeleton, one technique per kind of tree:

- broadleaf: single leaves, folded along the midrib, on the twigs and on twiglets too fine for
  the skeleton, turned toward the light;
- conifer: needle sprays, crossed ribbons along every fine shoot with needles in the texture;
- palm: each frond's two halves as strips out from the rachis, folded down and drooping.

Every mesh gets sky visibility from a density grid of the crown, so the inside darkens."""

import math

import numpy as np

UP = np.array([0.0, 1.0, 0.0])


def _unit(v):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-9)


def _perp(d):
    a = np.where(np.abs(d[..., 1:2]) < 0.9, UP, np.array([1.0, 0.0, 0.0]))
    return _unit(np.cross(d, a))


def _crown_centre(points):
    return np.median(points, axis=0)


def sky_visibility(points, occluders, cell=0.35, k=0.55, steps=10):
    """How much sky each point sees through `occluders` (points standing for leaf area):
    rays up the hemisphere through a density grid."""
    if len(occluders) == 0:
        return np.ones(len(points))
    lo = np.minimum(points.min(0), occluders.min(0)) - cell * 2
    idx = np.floor((occluders - lo) / cell).astype(int)
    dims = idx.max(0) + steps + 4
    grid = np.zeros(dims)
    np.add.at(grid, (idx[:, 0], idx[:, 1], idx[:, 2]), 1.0)
    grid /= max(np.percentile(grid[grid > 0], 90), 1.0)
    dirs = _unit(np.array([[0, 1, 0], [0.6, 0.8, 0], [-0.6, 0.8, 0], [0, 0.8, 0.6],
                           [0, 0.8, -0.6], [0.5, 0.5, 0.5], [-0.5, 0.5, -0.5],
                           [0.5, 0.5, -0.5], [-0.5, 0.5, 0.5]], float))
    vis = np.zeros(len(points))
    for d in dirs:
        acc = np.zeros(len(points))
        for s in range(1, steps + 1):
            q = np.floor((points + d * cell * s - lo) / cell).astype(int)
            inside = np.all((q >= 0) & (q < dims), axis=1)
            qq = np.clip(q, 0, dims - 1)
            acc += np.where(inside, grid[qq[:, 0], qq[:, 1], qq[:, 2]], 0.0)
        vis += np.exp(-k * acc)
    return vis / len(dirs)


def _segments(sk, max_radius):
    """Fine wood: (start, end, radius) of each segment whose radius is under `max_radius`."""
    pos = np.array(sk.pos)
    par = np.array(sk.parent)
    keep = (par >= 0) & (sk.radius < max_radius)
    return pos[par[keep]], pos[keep], sk.radius[keep]


# --- broadleaf ---------------------------------------------------------------------------

def leaves(sk, rng, per_segment=10, twig_radius=0.018, size=0.11, size_v=0.25, width=0.6,
           spread=0.22, tint=(1.0, 1.0, 1.0), tint_v=0.12, fold=0.18, droop=0.0,
           normal_bend=0.45, per_twiglet=7):
    """Single leaves `size` metres long on twiglets too fine for the skeleton: each fine
    segment sprouts twiglets about `spread` metres long, each holding `per_twiglet` leaves
    alternately along it and a cluster at its tip. Returns the leaf mesh, points standing for
    the leaf area, and the twiglets as a skeleton."""
    from skeleton import Skeleton

    a, b, _r = _segments(sk, twig_radius)
    if len(a) == 0:
        return None
    centre = _crown_centre(np.array(sk.pos)[sk.radius < twig_radius * 2])
    twigs = max(per_segment // per_twiglet, 1)
    m = len(a) * twigs
    seg = np.repeat(np.arange(len(a)), twigs)
    d = _unit(b - a)[seg]
    start = a[seg] + (b - a)[seg] * rng.random(m)[:, None]
    side = _unit(rng.normal(size=(m, 3)))
    side = _unit(side - d * np.sum(side * d, 1, keepdims=True))
    out = _unit(start - centre)
    tdir = _unit(d * 0.55 + side * 0.65 + out * 0.35 + UP * (0.25 - droop * 0.6))
    tlen = spread * (0.6 + 0.6 * rng.random(m))
    # Leaves along each twiglet, alternate, the last few crowded at its tip.
    k = per_twiglet
    s_along = (np.arange(k) + 0.6) / k
    s_along = s_along ** 0.7
    n = m * k
    tw = np.repeat(np.arange(m), k)
    sa = np.tile(s_along, m)
    base = start[tw] + tdir[tw] * (tlen[tw] * sa)[:, None]
    phase = np.tile(np.arange(k) * 2.4, m) + rng.random(n) * 0.6
    u = _perp(tdir[tw])
    v = np.cross(tdir[tw], u)
    around = u * np.cos(phase)[:, None] + v * np.sin(phase)[:, None]
    lout = _unit(base - centre)
    way = _unit(tdir[tw] * 0.55 + around * 0.75 + UP * (0.25 - droop) + lout * 0.2
                + rng.normal(size=(n, 3)) * 0.15)
    face = _unit(UP * 0.8 + lout * 0.6 + rng.normal(size=(n, 3)) * 0.3)
    face = _unit(face - way * np.sum(face * way, 1, keepdims=True))
    across = _unit(np.cross(way, face))
    length = size * (1 + size_v * (rng.random(n) * 2 - 1)) * (0.75 + 0.35 * sa)
    half = length * width * 0.5
    corners = [
        base - across * half[:, None],
        base + across * half[:, None],
        base + way * length[:, None] - across * half[:, None],
        base + way * length[:, None] + across * half[:, None],
        base + face * (fold * half)[:, None],
        base + way * length[:, None] + face * (fold * half)[:, None],
    ]
    uvs = [(0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0), (0.5, 1)]
    P = np.stack(corners, 1)
    bent = _unit(face * (1 - normal_bend) + lout * normal_bend)
    N = np.repeat(bent[:, None, :], 6, 1)
    UV = np.tile(np.array(uvs, float), (n, 1, 1))
    vis = sky_visibility(base, base)
    jitter = 1 + tint_v * (rng.random((n, 3)) * 2 - 1) * np.array([1.0, 0.7, 1.4])
    shade = (0.22 + 0.78 * vis)[:, None] * jitter * np.array(tint)
    C = np.repeat(shade[:, None, :], 6, 1)
    tri = np.array([(0, 4, 2), (4, 5, 2), (4, 1, 5), (1, 3, 5)])
    I = (np.arange(n)[:, None, None] * 6 + tri[None]).reshape(-1, 3)
    twiglets = Skeleton()
    radii = []
    for j in range(m):
        r0 = twiglets.add(start[j], -1)
        r1 = twiglets.add(start[j] + tdir[j] * tlen[j] * 0.5, r0)
        twiglets.add(start[j] + tdir[j] * tlen[j], r1)
        radii += [0.003, 0.0022, 0.0012]
    twiglets.radius = np.array(radii)
    return _pack(P, N, C, UV, I), base, twiglets


# --- conifer -----------------------------------------------------------------------------

def sprays(sk, rng, shoot_radius=0.03, width=0.16, repeat=0.45, droop=25.0, layers=2,
           tint=(1.0, 1.0, 1.0), tint_v=0.1, tips_only=0.0, normal_bend=0.5, star=False):
    """Needle sprays: crossed ribbons `width` metres across along every chain of fine shoots,
    the texture repeating every `repeat` metres; `tips_only` keeps just that many metres at
    each shoot's tip (a pine's brushes)."""
    chains = sk.chains()
    centre = _crown_centre(np.array(sk.pos))
    Ps, Ns, Cs, UVs, Is, bases = [], [], [], [], [], []
    count = 0
    for run in chains:
        pts = np.array([sk.pos[n] for n in run])
        rad = np.array([sk.radius[n] for n in run])
        keep = rad < shoot_radius
        if keep.sum() < 2:
            continue
        first = int(np.argmax(keep))
        pts = pts[max(first - 1, 0):]
        seglen = np.linalg.norm(np.diff(pts, axis=0), axis=1)
        dist = np.concatenate([[0], np.cumsum(seglen)])
        if tips_only:
            keep_tip = dist >= dist[-1] - tips_only
            start = max(int(np.argmax(keep_tip)) - 1, 0)
            pts, dist = pts[start:], dist[start:] - dist[start]
        if len(pts) < 2:
            continue
        d = np.gradient(pts, axis=0)
        d = _unit(d)
        side = _unit(np.cross(d, UP))
        side = np.where(np.linalg.norm(np.cross(d, UP), axis=1, keepdims=True) < 1e-3,
                        _perp(d), side)
        down = _unit(np.cross(side, d))  # roughly up, across the shoot
        w = width * (0.75 + 0.5 * rng.random())
        # Narrower toward the shoot's tip, as new growth is.
        taper = np.clip(1.15 - 0.5 * (dist / max(dist[-1], 1e-3)), 0.5, 1.0)
        tint_here = np.array(tint) * (1 + tint_v * (rng.random(3) * 2 - 1))
        for layer in range(layers):
            # Each ribbon tips down from level: one each side, as a spray hangs.
            # Two ribbons hang either side; a third, if asked, lies nearly level above them.
            ang = math.radians(droop) * (1 if layer % 2 == 0 else -1)
            if layer == 2:
                ang = math.radians(8)
            if star:
                # Ribbons crossing at the shoot's axis, as a brush seen from any side.
                ang = math.pi * layer / layers + rng.normal() * 0.1
            ang += rng.normal() * 0.15
            across = side * math.cos(ang) - down * math.sin(ang)
            left = pts - across * (w * 0.5 * taper)[:, None]
            right = pts + across * (w * 0.5 * taper)[:, None]
            face = _unit(np.cross(across, d))
            out = _unit(pts - centre)
            n = _unit(face * (1 - normal_bend) + out * normal_bend)
            v = dist / repeat
            P = np.concatenate([left, right])
            N = np.concatenate([n, n])
            UV = np.concatenate([np.stack([np.zeros_like(v), v], 1),
                                 np.stack([np.ones_like(v), v], 1)])
            C = np.tile(tint_here, (len(P), 1))
            k = len(pts)
            i = np.arange(k - 1)
            tris = np.concatenate([np.stack([i, i + k, i + 1], 1),
                                   np.stack([i + 1, i + k, i + k + 1], 1)]) + count
            Ps.append(P), Ns.append(N), Cs.append(C), UVs.append(UV), Is.append(tris)
            bases.append(pts)
            count += len(P)
    if not Ps:
        return None
    P, N, C, UV = map(np.concatenate, (Ps, Ns, Cs, UVs))
    base = np.concatenate(bases)
    vis = sky_visibility(P, base, cell=0.4, k=0.35)
    C = C * (0.25 + 0.75 * vis)[:, None]
    return (P.astype("f4"), N.astype("f4"), C.astype("f4"), np.concatenate(Is).astype("u4"),
            UV.astype("f4")), base


# --- palm --------------------------------------------------------------------------------

def fronds(sk, rng, leaflet=0.85, fold=28.0, droop=0.35, across_steps=3,
           tint=(1.0, 1.0, 1.0), tint_v=0.08):
    """Each frond's two halves as strips `leaflet` metres out from its rachis, folded down by
    `fold` degrees and drooping more toward their outer edge."""
    Ps, Ns, Cs, UVs, Is, bases = [], [], [], [], [], []
    count = 0
    for run in sk.chains():
        if sk.parent[run[0]] < 0:
            continue  # the stem
        pts = np.array([sk.pos[n] for n in run[1:]])
        if len(pts) < 3:
            continue
        d = _unit(np.gradient(pts, axis=0))
        side = _unit(np.cross(d, UP))
        up = _unit(np.cross(side, d))
        dist = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
        v = dist / dist[-1]
        tint_here = np.array(tint) * (1 + tint_v * (rng.random(3) * 2 - 1))
        for sgn in (-1.0, 1.0):
            rows = []
            for s in range(across_steps + 1):
                u = s / across_steps
                ang = math.radians(fold) + droop * u
                out = sgn * side * math.cos(ang) - up * math.sin(ang)
                reach = leaflet * u * (0.35 + 0.65 * np.sin(np.pi * np.clip(v, 0.02, 1)) ** 0.6)
                rows.append(pts + out * reach[:, None])
            k = len(pts)
            P = np.concatenate(rows)
            face = _unit(np.cross(d, rows[-1] - rows[0] + 1e-9)) * sgn
            N = np.tile(_unit(face * 0.6 + up * 0.4), (across_steps + 1, 1))
            UV = np.concatenate([np.stack([np.full(k, s / across_steps), v], 1)
                                 for s in range(across_steps + 1)])
            C = np.tile(tint_here, (len(P), 1))
            tris = []
            for s in range(across_steps):
                i = np.arange(k - 1) + s * k
                tris.append(np.stack([i, i + k, i + 1], 1))
                tris.append(np.stack([i + 1, i + k, i + k + 1], 1))
            Ps.append(P), Ns.append(N), Cs.append(C), UVs.append(UV)
            Is.append(np.concatenate(tris) + count)
            bases.append(pts)
            count += len(P)
    if not Ps:
        return None
    P, N, C, UV = map(np.concatenate, (Ps, Ns, Cs, UVs))
    vis = sky_visibility(P, P[::3], cell=0.5, k=0.25)
    C = C * (0.35 + 0.65 * vis)[:, None]
    return (P.astype("f4"), N.astype("f4"), C.astype("f4"), np.concatenate(Is).astype("u4"),
            UV.astype("f4")), np.concatenate(bases)


def _pack(P, N, C, UV, I):
    return (P.reshape(-1, 3).astype("f4"), N.reshape(-1, 3).astype("f4"),
            C.reshape(-1, 3).astype("f4"), I.astype("u4"), UV.reshape(-1, 2).astype("f4"))
