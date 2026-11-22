"""Far trees as impostors that light themselves: a tree is baked from many directions over a
hemi-octahedral grid into colour, normals, depth and leafiness, not into a picture of it lit.
Drawn, each pixel blends the four nearest baked views and is lit there and then, by the same
sun, sky, backlight and shadows as the meshes, so it changes with the light as the tree does."""

import math

import moderngl
import numpy as np

from render import _gl, context, look_at, ortho

GBUFFER_VS = """
#version 330
uniform mat4 view_proj;
in vec3 in_pos;
in vec3 in_normal;
in vec3 in_colour;
in vec2 in_uv;
in vec4 in_autumn;
out vec3 v_normal;
out vec3 v_colour;
out vec2 v_uv;
out float v_depth;
uniform vec3 near_point;
uniform vec3 toward;
uniform float span;
void main() {
    v_normal = in_normal;
    // Autumn baked as each leaf's turned share; a season's bake is redone as it moves.
    v_colour = mix(in_colour, in_autumn.rgb, in_autumn.a);
    v_uv = in_uv;
    v_depth = dot(near_point - in_pos, toward) / span;
    gl_Position = view_proj * vec4(in_pos, 1.0);
}
"""

GBUFFER_FS = """
#version 330
uniform sampler2D albedo;
uniform bool textured;
uniform float cutoff;
uniform float leafy;
uniform vec3 toward;
in vec3 v_normal;
in vec3 v_colour;
in vec2 v_uv;
in float v_depth;
layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_depth;
void main() {
    vec4 tex = textured ? texture(albedo, v_uv) : vec4(1.0);
    if (textured && tex.a < cutoff) discard;
    vec3 n = normalize(v_normal);
    // Two-sided leaves keep the side facing the baking camera.
    if (leafy > 0.0 && dot(n, toward) < 0.0) n = -n;
    o_albedo = vec4(v_colour * tex.rgb, 1.0);
    o_normal = vec4(n * 0.5 + 0.5, leafy);
    o_depth = vec4(v_depth, 0.0, 0.0, 1.0);
}
"""

DRAW_VS = """
#version 330
uniform mat4 view_proj;
uniform vec3 centre;
uniform float radius;
uniform vec3 right;
uniform vec3 up;
in vec2 in_corner;
out vec3 v_pos;
void main() {
    v_pos = centre + (right * in_corner.x + up * in_corner.y) * radius;
    gl_Position = view_proj * vec4(v_pos, 1.0);
}
"""

DRAW_FS = """
#version 330
uniform sampler2D atlas_albedo;
uniform sampler2D atlas_normal;
uniform sampler2D atlas_depth;
uniform sampler2DShadow shadow;
uniform int views;
uniform vec3 centre;
uniform float radius;
uniform vec3 look;        // from the tree toward the eye (or the light, when shadowing)
uniform mat4 light_proj;
uniform mat4 view_proj;
uniform vec3 sun;
uniform vec3 sun_colour;
uniform vec3 sky_colour;
uniform vec3 ground_colour;
uniform vec3 eye;
uniform float exposure;
uniform bool shadow_pass;
uniform float cutoff;
uniform float shadow_bias;
in vec3 v_pos;
out vec4 f_colour;

vec3 decode(vec2 g) {
    vec2 uv = (g + 0.5) / float(views) * 2.0 - 1.0;
    vec2 p = vec2(uv.x + uv.y, uv.x - uv.y) * 0.5;
    float y = 1.0 - abs(p.x) - abs(p.y);
    return normalize(vec3(p.x, y, p.y));
}

vec2 encode(vec3 d) {
    d.y = max(d.y, 0.0);
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 uv = vec2(d.x + d.z, d.x - d.z);
    return (uv * 0.5 + 0.5) * float(views) - 0.5;
}

void basis(vec3 d, out vec3 r, out vec3 u) {
    vec3 a = abs(d.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    r = normalize(cross(a, d));
    u = cross(d, r);
}

// One baked view's colour, normal, depth and leafiness at the world point on the card.
void sample_view(vec2 g, vec3 p, out vec4 a, out vec4 n, out float z) {
    vec3 d = decode(g);
    vec3 r, u;
    basis(d, r, u);
    vec2 st = vec2(dot(p - centre, r), dot(p - centre, u)) / radius * 0.5 + 0.5;
    st = clamp(st, 0.002, 0.998);
    vec2 cell = (g + st) / float(views);
    a = texture(atlas_albedo, cell);
    n = texture(atlas_normal, cell);
    z = texture(atlas_depth, cell).r;
}

float lit(vec3 world) {
    vec4 l = light_proj * vec4(world, 1.0);
    vec3 p = l.xyz / l.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0) return 1.0;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadow, 0));
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            sum += texture(shadow, vec3(p.xy + vec2(x, y) * texel * 1.3, p.z - shadow_bias));
    return sum / 25.0;
}

void main() {
    vec2 g = encode(normalize(look));
    vec2 g0 = clamp(floor(g), vec2(0.0), vec2(float(views) - 1.0));
    vec2 f = clamp(g - g0, 0.0, 1.0);
    vec4 acc_a = vec4(0.0);
    vec4 acc_n = vec4(0.0);
    float acc_z = 0.0;
    float wsum = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            vec2 gg = min(g0 + vec2(i, j), vec2(float(views) - 1.0));
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            vec4 a, n;
            float z;
            sample_view(gg, v_pos, a, n, z);
            float k = w * a.a;
            acc_a += vec4(a.rgb * k, k);
            acc_n += vec4((n.rgb * 2.0 - 1.0) * k, n.a * k);
            acc_z += z * k;
            wsum += w;
        }
    float coverage = acc_a.a / max(wsum, 1e-4);
    if (coverage < cutoff * 0.7) discard;
    vec3 base = acc_a.rgb / max(acc_a.a, 1e-4);
    // The averaged normal is shorter where the leaves under this pixel face many ways.
    vec3 mean_n = acc_n.rgb / max(acc_a.a, 1e-4);
    float agree = clamp(length(mean_n), 0.0, 1.0);
    vec3 nrm = mean_n / max(length(mean_n), 1e-4);
    float leafy = acc_n.a / max(acc_a.a, 1e-4);
    float depth01 = acc_z / max(acc_a.a, 1e-4);
    vec3 v = normalize(look);
    vec3 world = v_pos + v * radius - v * depth01 * 2.0 * radius;
    vec4 clip = view_proj * vec4(world, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    if (shadow_pass) {
        f_colour = vec4(0.0);
        return;
    }
    vec3 to_eye = normalize(eye - world);
    float shade = lit(world);
    float facing = dot(nrm, sun);
    float translucency = 0.55 * leafy;
    // Leaves facing every way take light as a wrapped average, not as one surface: the less
    // they agree, the more the response flattens toward it.
    float k = agree * agree;
    float direct = mix((facing * 0.5 + 0.5) * 0.5, max(facing, 0.0), k);
    float back = mix((0.5 - facing * 0.5) * 0.5, max(-facing, 0.0), k);
    float through = translucency * back
                  * (0.4 + 0.6 * pow(max(dot(-to_eye, sun), 0.0), 3.0));
    float upish = mix(0.5, nrm.y * 0.5 + 0.5, agree);
    vec3 ambient = mix(ground_colour, sky_colour, upish);
    vec3 c = base * (sun_colour * direct * shade + ambient)
           + base * vec3(1.25, 1.35, 0.6) * sun_colour * through * shade;
    c *= exposure;
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    c = pow(c, vec3(1.0 / 2.2));
    f_colour = vec4(c, clamp((coverage - cutoff * 0.7) / max(fwidth(coverage), 1e-4) + 0.5,
                             0.0, 1.0));
}
"""


class Impostor:
    def __init__(self, centre, radius, views, albedo, normal, depth):
        self.centre = centre
        self.radius = radius
        self.views = views
        self.albedo = albedo  # RGBA, linear colour, alpha coverage
        self.normal = normal  # RGB world normal packed 0-1, A leafiness
        self.depth = depth  # R: 0 at the bounding sphere's near side, 1 at its far side


def _decode(i, j, views):
    u = (i + 0.5) / views * 2 - 1
    v = (j + 0.5) / views * 2 - 1
    px, pz = (u + v) / 2, (u - v) / 2
    y = 1 - abs(px) - abs(pz)
    d = np.array([px, y, pz])
    return d / np.linalg.norm(d)


def _basis(d):
    a = np.array([0.0, 1.0, 0.0]) if abs(d[1]) < 0.99 else np.array([1.0, 0.0, 0.0])
    r = np.cross(a, d)
    r /= np.linalg.norm(r)
    return r, np.cross(d, r)


def bake(scene, views=12, cell=256, supersample=2):
    """An impostor of `scene` (a tree), baked from views x views directions over the
    upper hemisphere."""
    ctx = context()
    lo, hi = scene.bounds()
    centre = (lo + hi) / 2
    radius = float(np.linalg.norm(hi - lo) / 2) * 1.02
    big = cell * supersample
    prog = ctx.program(vertex_shader=GBUFFER_VS, fragment_shader=GBUFFER_FS)
    targets = [ctx.texture((big, big), 4, dtype="f4") for _ in range(3)]
    zbuf = ctx.depth_renderbuffer((big, big))
    fbo = ctx.framebuffer(targets, zbuf)
    vaos = []
    texs = []
    for (p, n, c, i, uv, autumn), mat in scene.parts:
        uv = np.zeros((len(p), 2), "f4") if uv is None else uv
        autumn = np.zeros((len(p), 4), "f4") if autumn is None else autumn
        vbo = ctx.buffer(np.hstack([p, n, c, uv, autumn]).astype("f4").tobytes())
        ibo = ctx.buffer(i.astype("u4").tobytes())
        vao = ctx.vertex_array(prog, [(vbo, "3f 3f 3f 2f 4f", "in_pos", "in_normal",
                                       "in_colour", "in_uv", "in_autumn")], ibo)
        tex = None
        if mat.texture is not None:
            h, w = mat.texture.shape[:2]
            tex = ctx.texture((w, h), 4, np.ascontiguousarray(mat.texture[::-1]).astype(
                "f4").tobytes(), dtype="f4")
            tex.build_mipmaps()
            tex.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
            texs.append(tex)
        vaos.append((vao, mat, tex, vbo, ibo))
    white = ctx.texture((1, 1), 4, np.ones(4, "f4").tobytes(), dtype="f4")
    size = views * cell
    atlas = [np.zeros((size, size, 4), np.float32) for _ in range(3)]
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)
    for j in range(views):
        for i in range(views):
            d = _decode(i, j, views)
            r, u = _basis(d)
            eye = centre + d * radius * 2
            view = look_at(eye, centre, up=u)
            vp = ortho(radius, radius, radius * 0.5, radius * 3.5) @ view
            prog["view_proj"].write(_gl(vp))
            prog["near_point"].value = tuple(centre + d * radius)
            prog["toward"].value = tuple(d)
            prog["span"].value = 2 * radius
            fbo.use()
            fbo.clear(0, 0, 0, 0, depth=1.0)
            for vao, mat, tex, _v, _i in vaos:
                (tex or white).use(0)
                prog["albedo"].value = 0
                prog["textured"].value = tex is not None
                prog["cutoff"].value = mat.cutoff
                prog["leafy"].value = 1.0 if mat.translucency > 0 else 0.0
                vao.render()
            for k in range(3):
                img = np.frombuffer(targets[k].read(), np.float32).reshape(big, big, 4)
                img = img.reshape(cell, supersample, cell, supersample, 4)
                if k == 0:
                    cov = img[..., 3].mean(axis=(1, 3))
                    rgb = (img[..., :3] * img[..., 3:]).sum(axis=(1, 3))
                    w = img[..., 3].sum(axis=(1, 3))[..., None]
                    out = np.dstack([rgb / np.maximum(w, 1e-6), cov])
                else:
                    alpha = np.frombuffer(targets[0].read(), np.float32).reshape(
                        big, big, 4)[..., 3].reshape(cell, supersample, cell, supersample)
                    w = alpha.sum(axis=(1, 3))[..., None]
                    out = (img * alpha[..., None]).sum(axis=(1, 3)) / np.maximum(w, 1e-6)
                # Atlas rows run bottom-up as GL textures do.
                atlas[k][j * cell:(j + 1) * cell, i * cell:(i + 1) * cell] = out
    for vao, _m, tex, vbo, ibo in vaos:
        vao.release(), vbo.release(), ibo.release()
    for t in texs + targets + [white]:
        t.release()
    zbuf.release(), fbo.release(), prog.release()
    # Spread colour and normal into empty texels so filtering never pulls in black.
    for k in (0, 1):
        atlas[k] = _dilate(atlas[k], atlas[0][..., 3] > 0)
    return Impostor(centre, radius, views, atlas[0], atlas[1], atlas[2])


def _dilate(img, filled, steps=4):
    out = img.copy()
    have = filled.copy()
    for _ in range(steps):
        acc = np.zeros_like(out[..., :3])
        cnt = np.zeros(have.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(np.roll(out[..., :3] * have[..., None], dy, 0), dx, 1)
            cnt += np.roll(np.roll(have, dy, 0), dx, 1)
        grow = (~have) & (cnt > 0)
        out[grow, :3] = acc[grow] / cnt[grow, None]
        have = have | grow
    return out


class Drawer:
    """Draws impostors inside render(): its shadow pass and its lit pass."""

    def __init__(self, impostors):
        self.items = impostors
        self.ctx = context()
        self.prog = None

    def _setup(self):
        ctx = self.ctx
        self.prog = ctx.program(vertex_shader=DRAW_VS, fragment_shader=DRAW_FS)
        quad = np.array([-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1], "f4")
        self.vbo = ctx.buffer(quad.tobytes())
        self.vao = ctx.vertex_array(self.prog, [(self.vbo, "2f", "in_corner")])
        self.textures = []
        for imp in self.items:
            ts = []
            for img in (imp.albedo, imp.normal, imp.depth):
                h, w = img.shape[:2]
                t = ctx.texture((w, h), 4, np.ascontiguousarray(img).astype("f4").tobytes(),
                                dtype="f4")
                t.build_mipmaps()
                t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
                ts.append(t)
            self.textures.append(ts)

    def draw(self, view_proj, look_from, shadow_pass, uniforms, shadow_tex=None):
        if self.prog is None:
            self._setup()
        p = self.prog
        p["view_proj"].write(_gl(view_proj))
        p["shadow_pass"].value = shadow_pass
        p["cutoff"].value = 0.5
        for k, v in uniforms.items():
            if k in p:
                if isinstance(v, np.ndarray) and v.shape == (4, 4):
                    p[k].write(_gl(v))
                else:
                    p[k].value = v
        if shadow_tex is not None:
            shadow_tex.use(0)
            p["shadow"].value = 0
        for imp, ts in zip(self.items, self.textures):
            for unit, t in enumerate(ts):
                t.use(unit + 1)
            p["atlas_albedo"].value = 1
            p["atlas_normal"].value = 2
            p["atlas_depth"].value = 3
            p["views"].value = imp.views
            p["centre"].value = tuple(imp.centre)
            p["radius"].value = imp.radius
            look = np.asarray(look_from, float) - imp.centre if not shadow_pass else np.asarray(
                look_from, float)
            look = look / np.linalg.norm(look)
            up_world = np.array([0.0, 1.0, 0.0])
            right = np.cross(up_world, look)
            if np.linalg.norm(right) < 1e-6:
                right = np.array([1.0, 0.0, 0.0])
            right /= np.linalg.norm(right)
            up = np.cross(look, right)
            p["look"].value = tuple(look)
            p["right"].value = tuple(right)
            p["up"].value = tuple(up)
            self.vao.render()

    def release(self):
        if self.prog is None:
            return
        for ts in self.textures:
            for t in ts:
                t.release()
        self.vao.release(), self.vbo.release(), self.prog.release()
        self.prog = None
