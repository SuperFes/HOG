"""Offscreen stills of meshes under a sun and sky, on the GPU through EGL."""

import math

import moderngl
import numpy as np
from PIL import Image, ImageDraw, ImageFont

_ctx = None


def context():
    global _ctx
    if _ctx is None:
        _ctx = moderngl.create_standalone_context(backend="egl")
    return _ctx


LIT_VS = """
#version 330
uniform mat4 view_proj;
uniform mat4 light_proj;
in vec3 in_pos;
in vec3 in_normal;
in vec3 in_colour;
in vec2 in_uv;
in vec4 in_autumn;
out vec3 v_pos;
out vec3 v_normal;
out vec3 v_colour;
out vec2 v_uv;
out vec4 v_autumn;
out vec4 v_light;
void main() {
    v_pos = in_pos;
    v_normal = in_normal;
    v_colour = in_colour;
    v_uv = in_uv;
    v_autumn = in_autumn;
    v_light = light_proj * vec4(in_pos, 1.0);
    gl_Position = view_proj * vec4(in_pos, 1.0);
}
"""

LIT_FS = """
#version 330
uniform sampler2DShadow shadow;
uniform sampler2D albedo;
uniform sampler2D turn_mask;
uniform bool textured;
uniform bool masked;
uniform float cutoff;
uniform float relief;  // bark: metres of height the texture's alpha spans; 0 for foliage
uniform vec3 sun;
uniform vec3 sun_colour;
uniform vec3 sky_colour;
uniform vec3 ground_colour;
uniform vec3 eye;
uniform float translucency;
uniform float exposure;
uniform int debug;  // 1 direct, 2 shadowing, 3 light through, as grey
in vec3 v_pos;
in vec3 v_normal;
in vec3 v_colour;
in vec2 v_uv;
in vec4 v_autumn;
in vec4 v_light;
out vec4 f_colour;

float lit() {
    vec3 p = v_light.xyz / v_light.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0) return 1.0;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadow, 0));
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            sum += texture(shadow, vec3(p.xy + vec2(x, y) * texel * 1.3, p.z - 0.0015));
    return sum / 25.0;
}

void main() {
    vec4 tex = textured ? texture(albedo, v_uv) : vec4(1.0);
    float alpha = 1.0;
    vec3 n0 = normalize(v_normal);
    float cavity = 1.0;
    if (textured && relief > 0.0) {
        // Bark relief from the height in alpha, by surface gradients (no tangents needed).
        vec3 dpdx = dFdx(v_pos);
        vec3 dpdy = dFdy(v_pos);
        float h = tex.a * relief;
        float dhx = dFdx(h);
        float dhy = dFdy(h);
        vec3 r1 = cross(dpdy, n0);
        vec3 r2 = cross(n0, dpdx);
        float det = dot(dpdx, r1);
        vec3 grad = sign(det) * (dhx * r1 + dhy * r2);
        n0 = normalize(abs(det) * n0 - grad);
        cavity = mix(0.45, 1.0, smoothstep(0.0, 0.8, tex.a));
    } else if (textured) {
        // Sharpened alpha to coverage: crisp edges that still antialias under MSAA.
        alpha = (tex.a - cutoff) / max(fwidth(tex.a), 1e-4) + 0.5;
        if (alpha <= 0.0) discard;
    }
    vec3 n = n0;
    vec3 to_eye = normalize(eye - v_pos);
    if (translucency > 0.0 && dot(n, to_eye) < 0.0) n = -n;
    float shade = lit();
    float facing = dot(n, sun);
    // Light through a leaf: strongest looking toward the sun through it, tinted yellow-green.
    float through = translucency * max(-facing, 0.0) * (0.4 + 0.6 * pow(max(dot(-to_eye, sun), 0.0), 3.0));
    float direct = max(facing, 0.0);
    float up = n.y * 0.5 + 0.5;
    vec3 ambient = mix(ground_colour, sky_colour, up);
    // Autumn: where on the leaf it turns first comes from the mask (edges, tip, between the
    // veins), how far it has turned from the leaf's own share.
    float turn = v_autumn.a;
    if (masked) {
        float m = texture(turn_mask, v_uv).r;
        turn = smoothstep(m - 0.08, m + 0.08, turn * 1.16 - 0.08);
    }
    vec3 base = mix(v_colour, v_autumn.rgb, turn) * tex.rgb * cavity;
    vec3 c = base * (sun_colour * direct * shade + ambient)
           + base * vec3(1.25, 1.35, 0.6) * sun_colour * through * shade;
    // A faint sheen on waxy leaves toward the sun.
    if (textured) {
        vec3 h = normalize(sun + to_eye);
        c += sun_colour * 0.04 * pow(max(dot(n, h), 0.0), 24.0) * shade;
    }
    if (debug > 0) {
        float d = debug == 1 ? direct : debug == 2 ? shade : debug == 3 ? through : direct * shade;
        f_colour = vec4(vec3(textured && relief <= 0.0 ? d : 0.0), clamp(alpha, 0.0, 1.0));
        return;
    }
    c *= exposure;
    c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    c = pow(c, vec3(1.0 / 2.2));
    f_colour = vec4(c, clamp(alpha, 0.0, 1.0));
}
"""

DEPTH_VS = """
#version 330
uniform mat4 light_proj;
in vec3 in_pos;
in vec2 in_uv;
out vec2 v_uv;
void main() { v_uv = in_uv; gl_Position = light_proj * vec4(in_pos, 1.0); }
"""
DEPTH_FS = """
#version 330
uniform sampler2D albedo;
uniform bool textured;
uniform float cutoff;
in vec2 v_uv;
void main() {
    // The true outline: a blurred mip's alpha falls under the cutoff and leaves stop casting.
    if (textured && textureLod(albedo, v_uv, 0.0).a < cutoff) discard;
}
"""


def look_at(eye, target, up=(0.0, 1.0, 0.0)):
    eye, target, up = map(lambda a: np.asarray(a, np.float64), (eye, target, up))
    f = target - eye
    f /= np.linalg.norm(f)
    s = np.cross(f, up)
    s /= np.linalg.norm(s)
    u = np.cross(s, f)
    m = np.identity(4)
    m[0, :3], m[1, :3], m[2, :3] = s, u, -f
    m[:3, 3] = -m[:3, :3] @ eye
    return m


def perspective(fov_y, aspect, near, far):
    f = 1.0 / math.tan(fov_y / 2)
    m = np.zeros((4, 4))
    m[0, 0], m[1, 1] = f / aspect, f
    m[2, 2] = (far + near) / (near - far)
    m[2, 3] = 2 * far * near / (near - far)
    m[3, 2] = -1.0
    return m


def ortho(half_w, half_h, near, far):
    m = np.identity(4)
    m[0, 0], m[1, 1] = 1 / half_w, 1 / half_h
    m[2, 2] = -2 / (far - near)
    m[2, 3] = -(far + near) / (far - near)
    return m


def _gl(m):
    return m.T.astype("f4").tobytes()


def ground_disc(radius, colour=(0.075, 0.10, 0.045), segments=64):
    ang = np.linspace(0, 2 * np.pi, segments, endpoint=False)
    p = np.concatenate([[[0, 0, 0]], np.stack([np.cos(ang) * radius, np.zeros_like(ang),
                                               np.sin(ang) * radius], -1)]).astype("f4")
    n = np.tile([0, 1, 0], (len(p), 1)).astype("f4")
    c = np.tile(colour, (len(p), 1)).astype("f4")
    i = np.array([(0, 1 + k, 1 + (k + 1) % segments) for k in range(segments)], np.uint32)
    return p, n, c, i


class Material:
    def __init__(self, texture=None, translucency=0.0, cutoff=0.5, turn_mask=None, relief=0.0):
        self.texture = texture  # RGBA float array, linear
        self.turn_mask = turn_mask  # where on the leaf autumn reaches first: 0 first, 1 last
        self.relief = relief  # bark: metres of height in the texture's alpha
        self.translucency = translucency
        self.cutoff = cutoff


class Scene:
    def __init__(self):
        # (positions, normals, colours, indices, uvs, autumn), Material; uvs and autumn may
        # be None.
        self.parts = []
        self.lod = []  # per part: what distance thinning needs (foliage.thinning), or None

    def add(self, mesh, material=None):
        if len(mesh[3]):
            mesh = tuple(mesh) + (None,) * (7 - len(mesh))
            self.parts.append((mesh[:6], material or Material()))
            self.lod.append(mesh[6])

    def bounds(self):
        lo = np.full(3, np.inf)
        hi = np.full(3, -np.inf)
        for (p, *_), _m in self.parts:
            lo = np.minimum(lo, p.min(axis=0))
            hi = np.maximum(hi, p.max(axis=0))
        return lo, hi


def render(scene, size=(512, 640), yaw=35.0, pitch=8.0, sun_dir=(0.45, 0.75, 0.35),
           fit=None, samples=8, sky=(0.60, 0.70, 0.85), view=None, impostors=None,
           fov_deg=30.0, ground=True, sun_colour=(3.2, 2.95, 2.6), debug=0, camera=None):
    """An image of `scene` framed to its bounds (or `fit` = (lo, hi)), the ground under it."""
    ctx = context()
    if fit is None and not scene.parts and impostors:
        fit = (min(i.centre - i.radius for i in impostors), max(i.centre + i.radius
                                                                 for i in impostors))
    lo, hi = fit if fit is not None else scene.bounds()
    centre = (lo + hi) / 2
    extent = hi - lo
    radius = max(extent[0], extent[2]) * 0.75 + 1.0
    full = Scene()
    full.parts = list(scene.parts)
    full.lod = list(scene.lod)
    if ground:
        full.add(ground_disc(5000.0 if camera is not None else max(radius * 3, 60.0)))

    w, h = size
    aspect = w / h
    fov = math.radians(fov_deg)
    height = extent[1]
    span = max(height / 2 * 1.08, max(extent[0], extent[2]) / 2 / aspect * 1.08, 0.5)
    dist = span / math.tan(fov / 2) + max(extent[0], extent[2]) / 2
    yr, pr = math.radians(yaw), math.radians(pitch)
    target = np.array([centre[0], lo[1] + height / 2, centre[2]])
    eye = target + dist * np.array([math.sin(yr) * math.cos(pr), math.sin(pr),
                                    math.cos(yr) * math.cos(pr)])
    if view is not None:
        # A close look: (point looked at, metres from it).
        target = np.asarray(view[0], np.float64)
        dist = float(view[1])
        eye = target + dist * np.array([math.sin(yr) * math.cos(pr), math.sin(pr),
                                        math.cos(yr) * math.cos(pr)])
    near, far = dist * 0.02, dist * 6
    if camera is not None:
        # A camera placed outright: (eye, point looked at).
        eye, target = (np.asarray(c, np.float64) for c in camera)
        near, far = 0.3, 4000.0
    view_proj = perspective(fov, aspect, near, far) @ look_at(eye, target)

    sun = np.asarray(sun_dir, np.float64)
    sun /= np.linalg.norm(sun)
    reach = max(np.linalg.norm(extent), 1.0) * 0.75 + radius
    light_proj = ortho(reach, reach, 0.0, reach * 4) @ look_at(target + sun * reach * 2, target)

    shadow_size = 4096
    depth = ctx.depth_texture((shadow_size, shadow_size))
    depth.compare_func = "<="
    depth.filter = (moderngl.LINEAR, moderngl.LINEAR)
    shadow_fbo = ctx.framebuffer(depth_attachment=depth)
    depth_prog = ctx.program(vertex_shader=DEPTH_VS, fragment_shader=DEPTH_FS)
    depth_prog["light_proj"].write(_gl(light_proj))
    lit_prog = ctx.program(vertex_shader=LIT_VS, fragment_shader=LIT_FS)
    lit_prog["view_proj"].write(_gl(view_proj))
    lit_prog["light_proj"].write(_gl(light_proj))
    lit_prog["sun"].value = tuple(sun)
    lit_prog["sun_colour"].value = tuple(sun_colour)
    lit_prog["sky_colour"].value = (0.55, 0.70, 1.0)
    lit_prog["ground_colour"].value = (0.18, 0.20, 0.12)
    lit_prog["exposure"].value = 1.0
    lit_prog["debug"].value = debug
    lit_prog["eye"].value = tuple(eye)

    textures = {}

    def texture_of(img):
        key = id(img)
        if key not in textures:
            h, w = img.shape[:2]
            data = np.ascontiguousarray(img[::-1].astype("f4"))
            t = ctx.texture((w, h), 4, data.tobytes(), dtype="f4")
            t.build_mipmaps()
            t.filter = (moderngl.LINEAR_MIPMAP_LINEAR, moderngl.LINEAR)
            t.anisotropy = 8.0
            textures[key] = t
        return textures[key]

    masks = {}
    white = ctx.texture((1, 1), 4, np.ones(4, "f4").tobytes(), dtype="f4")
    vaos = []
    for (p, n, c, i, uv, autumn), mat in full.parts:
        if uv is None:
            uv = np.zeros((len(p), 2), "f4")
        if autumn is None:
            autumn = np.zeros((len(p), 4), "f4")
        vbo = ctx.buffer(np.hstack([p, n, c, uv, autumn]).astype("f4").tobytes())
        ibo = ctx.buffer(i.astype("u4").tobytes())
        lit = ctx.vertex_array(lit_prog, [(vbo, "3f 3f 3f 2f 4f", "in_pos", "in_normal",
                                           "in_colour", "in_uv", "in_autumn")], ibo)
        shade = ctx.vertex_array(depth_prog, [(vbo, "3f 12x 12x 2f 16x", "in_pos", "in_uv")],
                                 ibo)
        tex = texture_of(mat.texture) if mat.texture is not None else None
        mask = None
        if mat.turn_mask is not None:
            m = mat.turn_mask
            if id(m) not in masks:
                masks[id(m)] = np.dstack([m, m, m, np.ones_like(m)]).astype("f4")
            mask = texture_of(masks[id(m)])
        vaos.append((lit, shade, mat, (tex, mask), (vbo, ibo)))

    def bind(prog, mat, texs):
        tex, mask = texs
        (tex or white).use(1)
        prog["albedo"].value = 1
        prog["textured"].value = tex is not None
        prog["cutoff"].value = mat.cutoff
        if "relief" in prog:
            prog["relief"].value = mat.relief
        if "masked" in prog:
            (mask or white).use(2)
            prog["turn_mask"].value = 2
            prog["masked"].value = mask is not None

    shadow_fbo.use()
    shadow_fbo.clear(depth=1.0)
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)
    for _lit, shade, mat, tex, _b in vaos:
        bind(depth_prog, mat, tex)
        shade.render()
    drawer = None
    if impostors:
        from impostor import Drawer
        drawer = Drawer(impostors)
        drawer.draw(light_proj, sun, True, {"light_proj": light_proj})

    colour = ctx.renderbuffer(size, components=4, samples=samples)
    zbuf = ctx.depth_renderbuffer(size, samples=samples)
    fbo = ctx.framebuffer(colour, zbuf)
    fbo.use()
    fbo.clear(*sky, 1.0, depth=1.0)
    depth.use(0)
    lit_prog["shadow"].value = 0
    alpha_to_coverage = 0x809E
    for lit, _shade, mat, tex, _b in vaos:
        lit_prog["translucency"].value = mat.translucency
        bind(lit_prog, mat, tex)
        coverage = tex[0] is not None and mat.relief <= 0
        if coverage:
            ctx.enable_direct(alpha_to_coverage)
        lit.render()
        if coverage:
            ctx.disable_direct(alpha_to_coverage)
    if drawer is not None:
        ctx.enable_direct(alpha_to_coverage)
        drawer.draw(view_proj, eye, False, {
            "light_proj": light_proj, "sun": tuple(sun), "sun_colour": tuple(sun_colour),
            "sky_colour": (0.55, 0.70, 1.0), "ground_colour": (0.18, 0.20, 0.12),
            "eye": tuple(eye), "exposure": 1.0, "debug": debug,
            "light_span": reach * 4, "shadow_bias": 0.1 / (reach * 4)}, shadow_tex=depth)
        ctx.disable_direct(alpha_to_coverage)
        drawer.release()

    resolved = ctx.framebuffer(ctx.renderbuffer(size, components=4))
    ctx.copy_framebuffer(resolved, fbo)
    img = Image.frombytes("RGBA", size, resolved.read(components=4)).transpose(
        Image.Transpose.FLIP_TOP_BOTTOM).convert("RGB")
    for lit, shade, _m, _t, (vbo, ibo) in vaos:
        lit.release(), shade.release(), vbo.release(), ibo.release()
    for t in textures.values():
        t.release()
    white.release()
    for r in (colour, zbuf, fbo, resolved, shadow_fbo, depth, depth_prog, lit_prog):
        r.release()
    return img


def _font(size):
    for name in ("DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                 "/usr/share/fonts/TTF/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def sheet(rows, title, row_labels, col_labels, path):
    """Panels in a grid, labelled, saved to `path`."""
    pw, ph = rows[0][0].size
    left, top = 150, 70
    out = Image.new("RGB", (left + pw * len(rows[0]), top + ph * len(rows)), (24, 24, 26))
    d = ImageDraw.Draw(out)
    d.text((12, 12), title, fill=(235, 235, 235), font=_font(26))
    for c, label in enumerate(col_labels):
        d.text((left + c * pw + 10, top - 28), label, fill=(200, 200, 200), font=_font(18))
    for r, row in enumerate(rows):
        d.text((12, top + r * ph + ph // 2 - 10), row_labels[r], fill=(200, 200, 200),
               font=_font(18))
        for c, img in enumerate(row):
            out.paste(img, (left + c * pw, top + r * ph))
    out.save(path)
    return out
