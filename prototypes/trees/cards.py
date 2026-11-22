"""Branch cards: a tree's leaves and twigs flattened onto a few textured planes per branch
cluster, for the distances where a pixel holds many leaves but the crown is still large on
screen. Each cluster gets three crossed planes facing the ways its leaves face most, and
every leaf goes on the plane it lies most nearly parallel to, so from any side a leaf is
drawn foreshortened about as it really is and only shifted along its plane's normal. Limbs
stay as mesh.

The planes hold what the impostor bake holds (colour, normal and leafiness, coverage), so a
card lights per texel as the leaves on it do."""

import math

import moderngl
import numpy as np

from impostor import GBUFFER_FS, GBUFFER_VS, _dilate, coverage_mips
from render import Material, Scene, _gl, context, look_at, ortho

LIMB = 0.04  # wood at least this wide (metres) stays as mesh


def _nearest(points, centres, chunk=65536):
    out = np.empty(len(points), np.int64)
    cc = (centres ** 2).sum(1)
    for i in range(0, len(points), chunk):
        q = points[i:i + chunk]
        out[i:i + chunk] = (cc[None] - 2 * q @ centres.T).argmin(1)
    return out


def clusters(points, across, iters=6, sample=40000, seed=0):
    """Labels grouping `points` into compact clusters about `across` metres wide: seeded one
    per occupied cell of that size, then k-means (on a sample, then every point)."""
    key = np.floor(points / across).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.ravel()
    centres = np.zeros((inv.max() + 1, 3))
    np.add.at(centres, inv, points)
    centres /= np.bincount(inv)[:, None]
    rng = np.random.default_rng(seed)
    some = points[rng.choice(len(points), min(sample, len(points)), replace=False)]
    for _ in range(iters):
        lab = _nearest(some, centres)
        n = np.bincount(lab, minlength=len(centres))
        sums = np.zeros_like(centres)
        np.add.at(sums, lab, some)
        live = n > 0
        centres = sums[live] / n[live, None]
    return np.unique(_nearest(points, centres), return_inverse=True)[1].ravel()


class Card:
    def __init__(self, centre, right, up, half, part_tris):
        self.centre, self.right, self.up, self.half = centre, right, up, half
        self.part_tris = part_tris  # per scene part, the triangle indices drawn on it


def plan(scene, across=1.5, limb=LIMB):
    """Cards for `scene`: per cluster of leaves, up to three planes, with the triangles of
    every non-limb part each one holds."""
    tris, normals, owners = [], [], []
    for k, ((p, n, c, idx, uv, autumn), mat) in enumerate(scene.parts):
        lod = scene.lod[k]
        sel = np.arange(len(idx))
        if mat.translucency <= 0 and lod is not None and lod["kind"] == "wood":
            sel = sel[lod["size"][idx[:, 0]] < limb]
        if not len(sel):
            continue
        v = p[idx[sel]].astype(float)
        fn = np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
        fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
        tris.append(v)
        normals.append(fn)
        owners.append(np.stack([np.full(len(sel), k), sel], 1))
    tris = np.concatenate(tris)
    normals = np.concatenate(normals)
    owners = np.concatenate(owners)
    centroid = tris.mean(axis=1)
    label = clusters(centroid, across)
    cards = []
    for c in range(label.max() + 1):
        m = np.flatnonzero(label == c)
        pts = tris[m].reshape(-1, 3)
        mean = pts.mean(0)
        # The planes face the ways the leaves face most: the axes of their orientation
        # tensor, blind to which side of a leaf is up.
        _, axes = np.linalg.eigh(normals[m].T @ normals[m])
        axes = axes.T[::-1]
        # Each triangle on the plane it lies most nearly parallel to.
        side = np.abs(normals[m] @ axes.T).argmax(1)
        for a in range(3):
            mine = m[side == a]
            if not len(mine):
                continue
            normal = axes[a]
            q = tris[mine].reshape(-1, 3)
            off = float(((centroid[mine] - mean) @ normal).mean())
            r, u = axes[(a + 1) % 3], axes[(a + 2) % 3]
            if abs(u[1]) < abs(r[1]):
                r, u = u, r
            if u[1] < 0:
                u = -u
            r = np.cross(u, normal)
            s, t = (q - mean) @ r, (q - mean) @ u
            centre = mean + normal * off + r * (s.max() + s.min()) / 2 + u * (t.max() + t.min()) / 2
            half = (max((s.max() - s.min()) / 2, 0.02), max((t.max() - t.min()) / 2, 0.02))
            part_tris = {}
            for k in np.unique(owners[mine, 0]):
                part_tris[int(k)] = owners[mine][owners[mine, 0] == k, 1]
            cards.append(Card(centre, r, u, half, part_tris))
    return cards


def bake(scene, cards, texel=0.015, supersample=2, pad=3, max_side=256):
    """Each card's colour, normal (+ leafiness) and coverage, packed in one atlas; and each
    card's place in it as (x0, y0, w, h) texels, rows top first."""
    ctx = context()
    prog = ctx.program(vertex_shader=GBUFFER_VS, fragment_shader=GBUFFER_FS)
    sizes = [(min(max(int(math.ceil(2 * c.half[0] / texel)), 4), max_side),
              min(max(int(math.ceil(2 * c.half[1] / texel)), 4), max_side)) for c in cards]
    # Shelf packing, tallest first.
    order = sorted(range(len(cards)), key=lambda i: -sizes[i][1])
    width = 2048
    place = [None] * len(cards)
    x = y = shelf = 0
    for i in order:
        w, h = sizes[i][0] + 2 * pad, sizes[i][1] + 2 * pad
        if x + w > width:
            x, y, shelf = 0, y + shelf, 0
        place[i] = (x + pad, y + pad, sizes[i][0], sizes[i][1])
        x += w
        shelf = max(shelf, h)
    height = 1 << int(math.ceil(math.log2(y + shelf)))
    albedo = np.zeros((height, width, 4), np.float32)
    normal = np.zeros((height, width, 4), np.float32)
    big = max_side * supersample
    targets = [ctx.texture((big, big), 4, dtype="f4") for _ in range(4)]
    zbuf = ctx.depth_renderbuffer((big, big))
    fbo = ctx.framebuffer(targets, zbuf)
    vbos, texs = [], {}
    for k, ((p, n, c, idx, uv, autumn), mat) in enumerate(scene.parts):
        uv = np.zeros((len(p), 2), "f4") if uv is None else uv
        autumn = np.zeros((len(p), 4), "f4") if autumn is None else autumn
        sv = np.tile(np.array([1, 0, 0, 0], "f4"), (len(p), 1))
        vbos.append(ctx.buffer(np.hstack([p, n, c, uv, autumn, sv]).astype("f4").tobytes()))
        if mat.texture is not None:
            hh, ww = mat.texture.shape[:2]
            t = ctx.texture((ww, hh), 4, np.ascontiguousarray(mat.texture[::-1]).astype(
                "f4").tobytes(), dtype="f4")
            t.build_mipmaps()
            t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
            texs[k] = t
    white = ctx.texture((1, 1), 4, np.ones(4, "f4").tobytes(), dtype="f4")
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)
    for card, (x0, y0, w, h) in zip(cards, place):
        normal_dir = np.cross(card.right, card.up)
        hw, hh = card.half
        # Texels square in metres: the tile spans what its size in texels does.
        hw, hh = w * texel / 2 if w < max_side else hw, h * texel / 2 if h < max_side else hh
        depth = 8.0
        view = look_at(card.centre + normal_dir * depth, card.centre, up=card.up)
        prog["view_proj"].write(_gl(ortho(hw, hh, 0.0, 2 * depth) @ view))
        prog["near_point"].value = tuple(card.centre + normal_dir * depth)
        prog["toward"].value = tuple(normal_dir)
        prog["span"].value = 2 * depth
        bw, bh = w * supersample, h * supersample
        fbo.use()
        ctx.viewport = (0, 0, bw, bh)
        fbo.clear(0, 0, 0, 0, depth=1.0)
        for k, sel in card.part_tris.items():
            mat = scene.parts[k][1]
            ibo = ctx.buffer(scene.parts[k][0][3][sel].astype("u4").tobytes())
            vao = ctx.vertex_array(prog, [(vbos[k], "3f 3f 3f 2f 4f 4f", "in_pos", "in_normal",
                                           "in_colour", "in_uv", "in_autumn", "in_sunvis")],
                                   ibo)
            texs.get(k, white).use(0)
            prog["albedo"].value = 0
            prog["textured"].value = k in texs
            prog["cutoff"].value = mat.cutoff
            prog["leafy"].value = 1.0 if mat.translucency > 0 else 0.0
            vao.render()
            vao.release(), ibo.release()

        def grab(k):
            a = np.frombuffer(fbo.read(viewport=(0, 0, bw, bh), components=4, attachment=k,
                                       dtype="f4"), np.float32).reshape(bh, bw, 4)
            return a[::-1].reshape(h, supersample, w, supersample, 4)
        a, nrm = grab(0), grab(1)
        cov = a[..., 3]
        wsum = cov.sum(axis=(1, 3))[..., None]
        albedo[y0:y0 + h, x0:x0 + w] = np.dstack(
            [(a[..., :3] * cov[..., None]).sum(axis=(1, 3)) / np.maximum(wsum, 1e-6),
             cov.mean(axis=(1, 3))])
        normal[y0:y0 + h, x0:x0 + w] = (nrm * cov[..., None]).sum(axis=(1, 3)) / np.maximum(
            wsum, 1e-6)
    ctx.screen.use() if ctx.screen else None
    for r in vbos + list(texs.values()) + targets + [white, zbuf, fbo, prog]:
        r.release()
    filled = albedo[..., 3] > 0
    albedo = _dilate(albedo, filled, 3, steps=pad + 1)
    normal = _dilate(normal, filled, 4, steps=pad + 1)
    return albedo, normal, place


def carded(scene, across=1.5, texel=0.015, limb=LIMB):
    """`scene` with its leaves and twigs on branch cards and its limbs as mesh; and the card
    count."""
    cards = plan(scene, across, limb)
    albedo, normal, place = bake(scene, cards, texel)
    out = Scene()
    for k, ((p, n, c, idx, uv, autumn), mat) in enumerate(scene.parts):
        lod = scene.lod[k]
        if mat.translucency <= 0 and lod is not None and lod["kind"] == "wood":
            keep = lod["size"][idx[:, 0]] >= limb
            if keep.any():
                out.add((p, n, c, idx[keep], uv, autumn), mat)
    H, W = albedo.shape[:2]
    P, N, UV, I = [], [], [], []
    for i, (card, (x0, y0, w, h)) in enumerate(zip(cards, place)):
        hw, hh = card.half
        hw, hh = w * texel / 2 if w < 256 else hw, h * texel / 2 if h < 256 else hh
        for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            P.append(card.centre + card.right * sx * hw + card.up * sy * hh)
            N.append(np.cross(card.right, card.up))
            # Rows run top first in the atlas; v runs up.
            UV.append(((x0 + (sx + 1) / 2 * w) / W, 1 - (y0 + (1 - sy) / 2 * h) / H))
        b = 4 * i
        I += [(b, b + 1, b + 2), (b, b + 2, b + 3)]
    leaf = next(m for _, m in scene.parts if m.translucency > 0)
    mat = Material(albedo, translucency=leaf.translucency, cutoff=0.5)
    mat.normal_map = normal
    mat.levels = coverage_mips([albedo, normal])
    out.add((np.array(P, "f4"), np.array(N, "f4"), np.ones((len(P), 3), "f4"),
             np.array(I, np.uint32), np.array(UV, "f4"), None), mat)
    return out, len(cards)
