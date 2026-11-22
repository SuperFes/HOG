"""How much of the sun each point of a tree sees through the tree itself, for any sun: depth
maps from many directions over the sky, each vertex tested against them, and the results fitted
per vertex as first-order spherical harmonics, vis(sun) ~ c0 + c1 . sun.

Four numbers that average correctly: an impostor texel or a far pixel holding many leaves gets
the share of them that is lit, not one leaf's answer, which a shadow map can't give once its
texels are coarser than a leaf."""

import math

import moderngl
import numpy as np

from render import DEPTH_FS, DEPTH_VS, _gl, context, look_at, ortho


def directions(n, lowest_deg=-90.0):
    """`n` directions spread evenly over the sphere, or down to `lowest_deg` above the
    horizon: light can come from anywhere, a lit city under a forest as well as the sun."""
    lo = math.sin(math.radians(lowest_deg))
    k = np.arange(n) + 0.5
    y = lo + (1 - lo) * k / n
    ring = np.sqrt(1 - y * y)
    az = k * math.pi * (3 - math.sqrt(5))
    return np.stack([ring * np.cos(az), y, ring * np.sin(az)], 1)


def _sample(depth, uv, z, bias, taps, spacing):
    size = depth.shape[0]
    lit = np.zeros(len(uv))
    for dy in range(-taps, taps + 1):
        for dx in range(-taps, taps + 1):
            q = uv * size + np.array([dx, dy]) * spacing
            i = np.clip(q.astype(int), 0, size - 1)
            lit += z - bias <= depth[i[:, 1], i[:, 0]]
    return lit / (2 * taps + 1) ** 2


def sun_visibility(scene, n=32, size=2048, bias_m=0.15, blur_m=0.03):
    """Per part of `scene`, (vertices, 4) float32: c0 and c1 of each vertex's visibility."""
    ctx = context()
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    reach = float(np.linalg.norm(hi - lo) / 2) * 1.05
    prog = ctx.program(vertex_shader=DEPTH_VS, fragment_shader=DEPTH_FS)
    tex = ctx.depth_texture((size, size))
    fbo = ctx.framebuffer(depth_attachment=tex)
    parts = []
    for (p, _n, _c, i, uv, _a), mat in scene.parts:
        uv = np.zeros((len(p), 2), "f4") if uv is None else uv
        vbo = ctx.buffer(np.hstack([p, uv]).astype("f4").tobytes())
        ibo = ctx.buffer(i.astype("u4").tobytes())
        vao = ctx.vertex_array(prog, [(vbo, "3f 2f", "in_pos", "in_uv")], ibo)
        t = None
        if mat.texture is not None:
            h, w = mat.texture.shape[:2]
            t = ctx.texture((w, h), 4, np.ascontiguousarray(mat.texture[::-1]).astype(
                "f4").tobytes(), dtype="f4")
            t.build_mipmaps()
            t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
        parts.append((p.astype(np.float64), vao, vbo, ibo, t, mat))
    white = ctx.texture((1, 1), 4, np.ones(4, "f4").tobytes(), dtype="f4")
    dirs = directions(n)
    vis = [np.zeros((n, len(p))) for p, *_ in parts]
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)
    span = reach * 4
    for k, d in enumerate(dirs):
        up = (0.0, 1.0, 0.0) if abs(d[1]) < 0.98 else (1.0, 0.0, 0.0)
        proj = ortho(reach, reach, 0.0, span) @ look_at(centre + d * reach * 2, centre, up)
        prog["light_proj"].write(_gl(proj))
        fbo.use()
        fbo.clear(depth=1.0)
        for _p, vao, _v, _i, t, mat in parts:
            (t or white).use(0)
            prog["albedo"].value = 0
            prog["textured"].value = t is not None
            prog["cutoff"].value = mat.cutoff
            vao.render()
        depth = np.frombuffer(tex.read(), np.float32).reshape(size, size)
        texel_m = 2 * reach / size
        for j, (p, *_rest) in enumerate(parts):
            h = np.hstack([p, np.ones((len(p), 1))]) @ proj.T
            q = h[:, :3] / h[:, 3:] * 0.5 + 0.5
            vis[j][k] = _sample(depth, q[:, :2], q[:, 2], bias_m / span, 1, blur_m / texel_m)
    for _p, vao, vbo, ibo, t, _m in parts:
        vao.release(), vbo.release(), ibo.release()
        if t is not None:
            t.release()
    white.release(), tex.release(), fbo.release(), prog.release()
    basis = np.hstack([np.ones((n, 1)), dirs])
    fit = np.linalg.pinv(basis)  # (4, n)
    return [in_range((fit @ v).T).astype("f4") for v in vis]


def in_range(sh):
    """Fits whose c0 + c1 . sun stays within 0-1 for every sun: c1 shrunk to fit. Those form
    a convex set, so any average of them (a shrunk impostor texel, a whole crown) stays in it,
    and the clamp a drawer would need never acts; clamping after averaging would brighten far
    trees wherever the fit overshoots, as it does toward the sun."""
    c0 = np.clip(sh[:, 0], 0.0, 1.0)
    c1 = sh[:, 1:]
    room = np.minimum(c0, 1.0 - c0)
    size = np.linalg.norm(c1, axis=1)
    c1 = c1 * np.minimum(1.0, room / np.maximum(size, 1e-9))[:, None]
    return np.hstack([c0[:, None], c1])
