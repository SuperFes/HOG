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
in vec4 in_sunvis;
out vec3 v_normal;
out vec3 v_colour;
out vec2 v_uv;
out float v_depth;
out vec4 v_sunvis;
uniform vec3 near_point;
uniform vec3 toward;
uniform float span;
void main() {
    v_normal = in_normal;
    // Autumn baked as each leaf's turned share; a season's bake is redone as it moves.
    v_colour = mix(in_colour, in_autumn.rgb, in_autumn.a);
    v_uv = in_uv;
    v_depth = dot(near_point - in_pos, toward) / span;
    v_sunvis = in_sunvis;
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
in vec4 v_sunvis;
layout(location = 0) out vec4 o_albedo;
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_depth;
layout(location = 3) out vec4 o_sunvis;
void main() {
    vec4 tex = textured ? texture(albedo, v_uv) : vec4(1.0);
    // The leaf's true outline, not a blurred mip of it, so tiny leaves keep their coverage;
    // the supersampling and the atlas mips do the averaging.
    if (textured && textureLod(albedo, v_uv, 0.0).a < cutoff) discard;
    vec3 n = normalize(v_normal);
    // Two-sided leaves keep the side facing the baking camera.
    if (leafy > 0.0 && dot(n, toward) < 0.0) n = -n;
    o_albedo = vec4(v_colour * tex.rgb, 1.0);
    o_normal = vec4(n * 0.5 + 0.5, leafy);
    o_depth = vec4(v_depth, 0.0, 0.0, 1.0);
    o_sunvis = v_sunvis;
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

# Shared by every impostor drawer. The host declares `views`, `centre`, `radius`, the atlas
# samplers, `sun`, `sun_colour`, `sky_colour`, `ground_colour`, `exposure`, `own_shadow`,
# `debug`, and ATLAS(cell), mapping a cell of one tree's atlas to texture coordinates.
IMPOSTOR_LIB = """
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

// One baked view's colour, normal, leafiness and own sun visibility where the ray from the
// eye (ro, rd) meets the tree as that view saw it: stepping along the ray to the height its
// depth map gives, so all views put each branch in the same place instead of ghosting it.
void sample_view(vec2 g, vec3 ro, vec3 rd, out vec4 a, out vec4 n, out vec4 sv, out vec3 hit) {
    vec3 d = decode(g);
    vec3 r, u;
    basis(d, r, u);
    float across = dot(rd, d);
    across = abs(across) < 0.05 ? (across < 0.0 ? -0.05 : 0.05) : across;
    float h = 0.0;  // height above the plane through the centre, toward the view
    vec2 cell;
    for (int it = 0; it < 3; ++it) {
        hit = ro + rd * ((h - dot(ro - centre, d)) / across);
        vec2 st = vec2(dot(hit - centre, r), dot(hit - centre, u)) / radius * 0.5 + 0.5;
        cell = (g + clamp(st, 0.002, 0.998)) / float(views);
        h = radius - texture(atlas_depth, ATLAS(cell)).r * 2.0 * radius;
    }
    hit = ro + rd * ((h - dot(ro - centre, d)) / across);
    a = texture(atlas_albedo, ATLAS(cell));
    n = texture(atlas_normal, ATLAS(cell));
    sv = texture(atlas_sunvis, ATLAS(cell));
}

vec3 tone(vec3 c) {
    c *= exposure;
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    return pow(c, vec3(1.0 / 2.2));
}

// The four blended views lit, in the tree's own frame: `shade` is the sunlight left after
// whatever lies outside the tree, `sky` the share of the sky it leaves open.
vec3 light_views(vec4 sa[4], vec4 sn[4], vec4 ssv[4], float sk[4], float acc_a, vec3 to_eye,
                 float shade, float sky, inout float dbg) {
    float toward_sun = 0.4 + 0.6 * pow(max(dot(-to_eye, sun), 0.0), 3.0);
    // Each view is lit with its own normal and the lit colours blended: the views see
    // different leaves, so blending normals first would cancel them and lose the highlights.
    // Leaves facing every way still face the viewer, as two-sided leaves are seen from their
    // near side: averaged over such leaves, how much sun lands on their front and how much
    // comes through from behind (a fit to the cosine-weighted hemisphere's average).
    float cv = dot(to_eye, sun);
    float spread_front = (1.0 + cv) * (2.0 + cv) / 9.0;
    float spread_back = (1.0 - cv) * (2.0 - cv) / 9.0;
    vec3 c = vec3(0.0);
    for (int s = 0; s < 4; ++s) {
        vec3 base = sa[s].rgb;
        vec3 mean_n = sn[s].rgb * 2.0 - 1.0;
        // Shorter where a shrunk texel holds leaves facing many ways.
        float agree = clamp(length(mean_n), 0.0, 1.0);
        vec3 nrm = mean_n / max(length(mean_n), 1e-4);
        float facing = dot(nrm, sun);
        float k = agree * agree;
        float direct = mix(spread_front, max(facing, 0.0), k);
        float back = mix(spread_back, max(-facing, 0.0), k);
        float through = 0.55 * sn[s].a * back * toward_sun;
        float own = own_shadow ? clamp(dot(ssv[s], vec4(1.0, sun)), 0.0, 1.0) : 1.0;
        dbg += (debug == 1 ? direct : debug == 2 ? own * shade : debug == 3 ? through : direct * own * shade) * sk[s];
        vec3 ambient = mix(ground_colour, sky_colour, mix(0.5, nrm.y * 0.5 + 0.5, agree)) * sky;
        vec3 h = normalize(sun + to_eye);
        vec3 dark = base * ambient;
        vec3 sunny = dark + base * sun_colour * direct
                   + base * vec3(1.25, 1.35, 0.6) * sun_colour * through
                   + sun_colour * 0.04 * pow(max(dot(nrm, h), 0.0), 24.0) * k;
        // A texel stands for many leaves, a share of them in sun and the rest in shade; each
        // kind goes through the tone curve on its own, as a pixel showing them would, since
        // tone-mapping their average comes out brighter and flatter than the tree.
        float share = own * shade;
        c += (tone(sunny) * share + tone(dark) * (1.0 - share)) * sk[s];
    }
    return c / max(acc_a, 1e-4);
}

// Coverage drawn from a blend of views: magnified, edges are sharpened as alpha-tested leaves
// are; shrunk, a texel holds many leaves and gaps, and its coverage is drawn as it is, so the
// crown stays see-through.
float impostor_alpha(float coverage, vec3 pos, float cutoff) {
    float cell_texels = float(textureSize(atlas_albedo, 0).y) / float(views);
    float texels = length(fwidth(pos)) / (2.0 * radius) * cell_texels;
    float minified = clamp(log2(max(texels, 1e-4)), 0.0, 1.0);
    float sharp = (coverage - cutoff * 0.7) / max(fwidth(coverage), 1e-4) + 0.5;
    return clamp(mix(sharp, coverage, minified), 0.0, 1.0);
}
"""

DRAW_FS = """
#version 330
uniform sampler2D atlas_albedo;
uniform sampler2D atlas_normal;
uniform sampler2D atlas_depth;
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
uniform sampler2D shadow_raw;
uniform float light_span;     // metres the light's depth range covers
uniform float leaf_density;   // how fast sunlight thins per metre inside the crown
uniform sampler2D atlas_sunvis;  // the leaves' own sun visibility, if baked
uniform bool own_shadow;
uniform int debug;            // 1 direct, 2 shadowing, 3 light through, as grey
in vec3 v_pos;
out vec4 f_colour;
#define ATLAS(c) (c)
""" + IMPOSTOR_LIB + """
// Sunlight left after passing through the crown: the shadow map holds the first leaf toward
// the sun, and past it light thins by leaf density, as through cloud, so leaves just behind
// the surface shade each other while the deep inside goes dark.
float lit(vec3 world) {
    vec4 l = light_proj * vec4(world, 1.0);
    vec3 p = l.xyz / l.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0) return 1.0;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadow_raw, 0));
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            float first = texture(shadow_raw, p.xy + vec2(x, y) * texel * 1.3).r;
            // With the tree's own shading baked, only what lies beyond its crown shades it.
            float skip = own_shadow ? 2.0 * radius : shadow_bias * light_span;
            float inside = max((p.z - first) * light_span - skip, 0.0);
            sum += exp(-leaf_density * inside);
        }
    return sum / 25.0;
}

void main() {
    vec2 g = encode(normalize(look));
    vec2 g0 = clamp(floor(g), vec2(0.0), vec2(float(views) - 1.0));
    vec2 f = clamp(g - g0, 0.0, 1.0);
    vec4 sa[4];
    vec4 sn[4];
    vec4 ssv[4];
    float sk[4];
    float acc_a = 0.0;
    vec3 acc_hit = vec3(0.0);
    // Shadow maps look along the light, parallel; the eye looks from a point.
    vec3 ro = shadow_pass ? v_pos + look * radius * 4.0 : eye;
    vec3 rd = shadow_pass ? -look : normalize(v_pos - eye);
    float wsum = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int s = j * 2 + i;
            vec2 gg = min(g0 + vec2(i, j), vec2(float(views) - 1.0));
            float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            vec3 hit;
            sample_view(gg, ro, rd, sa[s], sn[s], ssv[s], hit);
            sk[s] = w * sa[s].a;
            acc_a += sk[s];
            acc_hit += hit * sk[s];
            wsum += w;
        }
    float alpha = impostor_alpha(acc_a / max(wsum, 1e-4), v_pos, cutoff);
    if (shadow_pass) {
        // No coverage to blend in a depth map: dither it, and the filtered lookup evens it.
        float h = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
        if (alpha <= h * 0.999) discard;
    } else if (alpha <= 0.0) {
        discard;
    }

    vec3 world = acc_hit / max(acc_a, 1e-4);
    vec4 clip = view_proj * vec4(world, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    if (shadow_pass) {
        f_colour = vec4(0.0);
        return;
    }
    float dbg = 0.0;
    vec3 c = light_views(sa, sn, ssv, sk, acc_a, normalize(eye - world), lit(world), 1.0, dbg);
    if (debug > 0) {
        f_colour = vec4(vec3(dbg / max(acc_a, 1e-4)), alpha);
        return;
    }
    f_colour = vec4(c, alpha);
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
        self.leaf_density = 1.5  # sunlight's thinning per metre inside the crown
        self.sunvis = None  # c0 and c1 of the leaves' own sun visibility, if baked


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
    targets = [ctx.texture((big, big), 4, dtype="f4") for _ in range(4)]
    zbuf = ctx.depth_renderbuffer((big, big))
    fbo = ctx.framebuffer(targets, zbuf)
    vaos = []
    texs = []
    sunvis = getattr(scene, "sunvis", None) or [None] * len(scene.parts)
    for ((p, n, c, i, uv, autumn), mat), sv in zip(scene.parts, sunvis):
        uv = np.zeros((len(p), 2), "f4") if uv is None else uv
        autumn = np.zeros((len(p), 4), "f4") if autumn is None else autumn
        # Without a baked answer, every point sees the whole sun.
        sv = np.tile(np.array([1, 0, 0, 0], "f4"), (len(p), 1)) if sv is None else sv
        vbo = ctx.buffer(np.hstack([p, n, c, uv, autumn, sv]).astype("f4").tobytes())
        ibo = ctx.buffer(i.astype("u4").tobytes())
        vao = ctx.vertex_array(prog, [(vbo, "3f 3f 3f 2f 4f 4f", "in_pos", "in_normal",
                                       "in_colour", "in_uv", "in_autumn", "in_sunvis")], ibo)
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
    atlas = [np.zeros((size, size, 4), np.float32) for _ in range(4)]
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
            for k in range(4):
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
    filled = atlas[0][..., 3] > 0
    for k, ch in ((0, 3), (1, 3), (3, 4)):
        atlas[k] = _dilate(atlas[k], filled, ch)
    # Depth spreads into the gaps too, so a ray stepping through one lands near the leaves
    # around it; far from any, the centre plane.
    atlas[2] = _dilate(atlas[2], filled, 1, steps=8)
    atlas[2][..., 0] = np.where(atlas[2][..., 0] > 0, atlas[2][..., 0], 0.5)
    imp = Impostor(centre, radius, views, atlas[0], atlas[1], atlas[2])
    imp.sunvis = atlas[3] if getattr(scene, "sunvis", None) else None
    return imp


def _dilate(img, filled, ch=3, steps=4):
    out = img.copy()
    have = filled.copy()
    for _ in range(steps):
        acc = np.zeros_like(out[..., :ch])
        cnt = np.zeros(have.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(np.roll(out[..., :ch] * have[..., None], dy, 0), dx, 1)
            cnt += np.roll(np.roll(have, dy, 0), dx, 1)
        grow = (~have) & (cnt > 0)
        out[grow, :ch] = acc[grow] / cnt[grow, None]
        have = have | grow
    return out


def coverage_mips(maps):
    """Mip chains for impostor maps (albedo with coverage in alpha first), each texel of a
    level the coverage-weighted mean of the four below it: a shrunk texel shows the leaves
    it holds, not the padding spread into the empty texels around them."""
    chains = [[m.astype(np.float32)] for m in maps]
    while min(chains[0][-1].shape[:2]) > 1:
        a = chains[0][-1]
        h, w = a.shape[0] // 2, a.shape[1] // 2
        cov = a[:h * 2, :w * 2, 3].reshape(h, 2, w, 2)
        weight = cov.sum(axis=(1, 3))[..., None]
        for chain in chains:
            m = chain[-1][:h * 2, :w * 2].reshape(h, 2, w, 2, -1)
            plain = m.mean(axis=(1, 3))
            covered = (m * cov[..., None]).sum(axis=(1, 3)) / np.maximum(weight, 1e-8)
            chain.append(np.where(weight > 1e-6, covered, plain))
        chains[0][-1][..., 3] = cov.mean(axis=(1, 3))
    return chains


class Drawer:
    """Draws impostors inside render(): its shadow pass and its lit pass."""

    # Plain mips average in the empty texels past the padding (no normal, no sun), which
    # darkens far impostors by chance; weighted mips hold only the leaves.
    weighted_mips = True

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
            sunvis = imp.sunvis if imp.sunvis is not None else np.ones_like(imp.albedo) * [
                1, 0, 0, 0]
            maps = (imp.albedo, imp.normal, imp.depth, sunvis)
            for levels in coverage_mips(maps) if self.weighted_mips else [[m] for m in maps]:
                h, w = levels[0].shape[:2]
                t = ctx.texture((w, h), 4, np.ascontiguousarray(levels[0]).astype(
                    "f4").tobytes(), dtype="f4")
                t.build_mipmaps()
                for lvl, m in enumerate(levels[1:], 1):
                    t.write(np.ascontiguousarray(m).astype("f4").tobytes(), level=lvl)
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
            raw = self.ctx.sampler(texture=shadow_tex, compare_func="",
                                   filter=(moderngl.NEAREST, moderngl.NEAREST))
            shadow_tex.use(4)
            raw.use(4)
            p["shadow_raw"].value = 4
        for imp, ts in zip(self.items, self.textures):
            for unit, t in enumerate(ts[:3]):
                t.use(unit + 1)
            p["atlas_albedo"].value = 1
            p["atlas_normal"].value = 2
            p["atlas_depth"].value = 3
            if "atlas_sunvis" in p:
                ts[3].use(6)
                p["atlas_sunvis"].value = 6
                p["own_shadow"].value = imp.sunvis is not None
            p["views"].value = imp.views
            p["centre"].value = tuple(imp.centre)
            p["radius"].value = imp.radius
            if "leaf_density" in p:
                p["leaf_density"].value = imp.leaf_density
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
        if shadow_tex is not None:
            raw.release()

    def release(self):
        if self.prog is None:
            return
        for ts in self.textures:
            for t in ts:
                t.release()
        self.vao.release(), self.vbo.release(), self.prog.release()
        self.prog = None
