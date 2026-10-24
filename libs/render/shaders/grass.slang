import atmosphere;
import bend;
import grass_lattice;
import gusts;
import lush;
import shadow;
import tone;
import water;

// Blades grown on the ground's own patches: each one on its own spot of the world's lattice, so
// every machine grows the same field whichever patches draw it, swaying with the weather's wind.

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 bend_up;
    // x the planet's radius and y the faked one drawing bends it onto; z seconds of sway so far,
    // w the radians across a pixel.
    float4 bend_radii;
    // xyz the wind near the ground, m/s; w (as bits) the patch's index among those grown.
    float4 wind;
    // xyz the patch's origin from the eye, in metres; w (as bits) the levels into it its blades
    // are sown, the bytes before the patch's vertices in what's bound times 64, and the metres
    // across its grid cells times 256 << 14.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint biome_count = 14;

struct Settings {
    // Each biome's blades: rgb their colour, w their height in metres, none for no grass.
    float4 blade[biome_count];
    // x the share of blades grown, y their width in metres, z the chance each clump corner is on,
    // w the share of reed stems standing in the band at the waterline.
    float4 spread[biome_count];
    // How it thins: x blades a square metre up close, y the metres that density holds to before
    // falling with the square of distance, z the metres every blade has shrunk away by.
    float4 tier;
    // x the level of the lattice the blades stand on, y the seed its dice are cast from.
    uint4 lattice;
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// The pool's patch vertices, as render::TerrainPool lays them out.
[[vk::binding(1, 0)]] ByteAddressBuffer vertices;
[[vk::binding(2, 0)]] ConstantBuffer<Air> air;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] Sampler2D transmittance_table;
[[vk::binding(5, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(6, 0)]] Sampler2D cloud_shadow;

static const uint patch_cells = 32;
static const uint patch_side = 33;
static const uint patch_depth = 5; // levels from a patch's cell to its grid cells
static const uint vertex_stride = 36; // bytes, as terrain::PatchVertex lies

// Blades keep off ground this close to sea level, so the surf stays clear.
static const float shore = 0.15;
static const float pioneers = 0.9; // of the open ground the grass takes where trees are gone
// Of the reach, the last share over which blades end, as terrain.slang's grass_fraying.
static const float fraying = 0.4;
// How much darker a field of grass shows than its ground's colour, as terrain.slang's
// grass_field_shade: what blades melting into the ground wear.
static const float field_shade = 0.7;
// Levels up the lattice a blade is looked at for whether it's kept, at most.
static const uint most_kept_up = 16;

// Reeds stand where the ground sits this close over the water, stiff stems with a cattail head:
// the quad is cut as wide as the head, and the stem trimmed from it in the fragment shader.
static const float reed_low = 0.03;
static const float reed_high = 0.7;
static const float reed_height = 1.7; // metres, before each stem's own variance
static const float reed_width = 0.05; // the head's; the stem is trimmed thinner
static const float head_cut = 0.78;   // of the stem's height, where the head begins
static const float3 reed_stem = float3(0.06, 0.11, 0.035);
static const float3 reed_head = float3(0.14, 0.07, 0.03);

struct Corner {
    float3 position;
    float3 normal;
    uint biome;
    // The biome the ground about it fades into, and that biome's share.
    uint blend_biome;
    float blend;
    // As terrain::Cover: the ground a fire left black, and of the trees it would hold those gone
    // and those fallen.
    float scar;
    float gone;
    float fallen;
};

Corner corner_at(uint bias, uint index) {
    const uint at = bias + (index * vertex_stride);
    Corner corner;
    corner.position = asfloat(vertices.Load3(at));
    corner.normal = asfloat(vertices.Load3(at + 12));
    corner.biome = min(vertices.Load(at + 24) & 0xffu, biome_count - 1);
    const uint blend = vertices.Load(at + 32);
    corner.blend_biome = min(blend & 0xffu, biome_count - 1);
    corner.blend = float((blend >> 8) & 0xffu) / 255.0;
    const uint cover = vertices.Load(at + 28) >> 16;
    corner.scar = float(cover & 0xffu) / 255.0;
    corner.gone = float((cover >> 12) & 0xfu) / 15.0;
    corner.fallen = float((cover >> 8) & 0xfu) / 15.0;
    return corner;
}

uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

float unit(uint at) {
    return float(hashed(at) & 0xffffu) / 65536.0;
}

// The two triangles of a blade's quad: root left and right, then tip left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };

struct Blade {
    float4 position : SV_Position;
    float3 colour;
    float3 head; // the colour above the cut, for reeds
    float3 normal;
    float3 from_eye;
    float3 drawn;    // from the eye, bent
    float height;    // above sea level
    float t_up;      // 0 at the root, 1 at the tip
    float cut;       // of t_up, where the head's colour takes over; past 1 for plain blades
    float side;      // across the quad, -0.5 to 0.5
    float stem_half; // half-width kept below the cut; the rest is trimmed away
};

// Nothing: a point's triangles cover no pixels.
Blade nothing() {
    Blade gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.head = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    gone.t_up = 0.0;
    gone.cut = 2.0;
    gone.side = 0.0;
    gone.stem_half = 1.0;
    return gone;
}

// How far the tip is held and swayed by the wind: held over in strong wind, swaying most in a
// breeze, still in still air. Gusts lay the grass over as they pass, and blades sway in waves
// running downwind.
float3 blown(float3 root, float height, uint hash) {
    const float mean = length(push.wind.xyz);
    if (mean < 1e-3) {
        return float3(0.0);
    }
    const float3 along = push.wind.xyz / mean;
    const GustFrame frame = gust_frame(lighting);
    const float4 fields = gust_fields(root, frame);
    const float gust = gust_of(fields.x);
    const float speed = gusting(mean, gust);
    // Each blade a little apart from its neighbours, as many as ahead of the wave as behind it.
    const float phase =
        sway_phase(lighting.downwind.w, root, frame, fields) + (1.2 * (unit(hash) - 0.5));
    return along * (height * (laid_over(speed) + (swung(gust, fields) * swaying(speed) * swing(phase, fields))));
}

// A blade as grown for the frame: where it stands and how it's shaped and coloured.
struct Grown {
    float3 root;   // from the eye, unbent
    float above;   // the root's height above sea level
    float3 across; // the quad's full width at the root, side to side
    float3 rise;   // root to tip, standing
    float3 tip;    // how far the wind carries the tip
    float3 normal; // of the ground under it
    uint biome;
    bool reed;
    float itself; // its true width's share of that drawn
    float tone;   // its own shade, about 1
};

// Of the blades standing for cells `stood_up` levels up the lattice, the share kept where the field
// is thinned to a blade a cell `thinned_up` levels up: all of those standing just short of it, and
// a share of those standing a level shorter.
float kept_share(uint stood_up, float thinned_up) {
    const float short_of = min(float(stood_up) + 1.0 - thinned_up, 2.0);
    return (exp2(2.0 * short_of) - 1.0) / 3.0;
}

// The patches grown on this frame: each one's cell, as face, i, j and level; with the level, in
// eighths, how far below its sowing depth the patch thins at least, as grass_patch_level packs it.
[[vk::binding(10, 0)]] StructuredBuffer<uint4> grass_patches;

// The patch's blade `instance` as it grows this frame, if it grows at all.
bool grow(uint instance, out Grown grown) {
    grown = (Grown)0;
    const uint packed = asuint(push.origin.w);
    const uint depth = packed & 63u;
    const uint bias = (packed >> 6) & 0xffu;
    const float spacing = float((packed >> 14) & 0xfffu) / 256.0;
    const float4 tier = settings.tier;
    const float fade_far = tier.z;
    const uint lattice = settings.lattice.x;
    const uint seed = settings.lattice.y;
    const uint4 record = grass_patches[asuint(push.wind.w)];
    const uint patch_level = record.w & 0xffu;
    // How far up the lattice the patch thins at least, where it's thickest.
    const float least_up = float(lattice - patch_level - patch_depth) -
                           (float(int(record.w >> 8) - 128) / 8.0) - 0.01;

    // The blades standing on the lattice for the patch's cells `depth` levels in, the same blade in
    // the same spot whichever patch grows it, sown in layers as the field thickens: the patch
    // cell's own, then a level at a time the blades of each cell's children it doesn't stand for.
    // A layer's blades all stand for cells as far up, so whole waves of them go or stay together.
    if (instance >= (1u << (2 * depth))) {
        return false;
    }
    FaceCell sown = { record.x, record.y, record.z, patch_level };
    uint dice;
    uint stood_up;
    if (instance == 0) {
        stood_up = (lattice - patch_level) + stands_up(sown, seed, most_kept_up, dice);
    } else {
        const uint above = firstbithigh(instance) / 2;
        stood_up = lattice - (patch_level + above + 1);
        // Most layers are thinned out, even where the patch is thickest.
        if (kept_share(stood_up, least_up) <= 0.0) {
            return false;
        }
        const uint into = instance - (1u << (2 * above));
        const uint cell = into / 3;
        const FaceCell parent = { record.x, (record.y << above) + (cell & ((1u << above) - 1u)),
                                  (record.z << above) + (cell >> above), patch_level + above };
        sown = unpicked_child(parent, into % 3, seed, dice);
    }
    const float thinned_by = 0.75 * unit(dice);
    if (kept_share(stood_up, least_up) <= thinned_by) {
        return false;
    }
    const FaceCell blade = standing_for(sown, lattice, seed);
    const uint hash = grass_hash(blade, seed);
    const float2 spot = grass_spot(hash);
    const uint to_lattice = lattice - patch_level;
    // Lattice cells to a grid cell, as a fraction of one.
    const float to_grid = exp2(float(patch_level + patch_depth) - float(lattice));
    const float2 on_grid =
        min((float2(float(blade.i - (record.y << to_lattice)),
                    float(blade.j - (record.z << to_lattice))) + spot) * to_grid,
            float(patch_cells) - 1e-4);

    // Rooted within its grid cell, on ground interpolated between the cell's corners.
    const uint2 cell = uint2(on_grid);
    const float u = on_grid.x - float(cell.x);
    const float v = on_grid.y - float(cell.y);
    const uint at = (cell.y * patch_side) + cell.x;
    const Corner c00 = corner_at(bias, at);
    const Corner c10 = corner_at(bias, at + 1);
    const Corner c01 = corner_at(bias, at + patch_side);
    const Corner c11 = corner_at(bias, at + patch_side + 1);
    const Corner near = v < 0.5 ? (u < 0.5 ? c00 : c10) : (u < 0.5 ? c01 : c11);
    const float3 ground = lerp(lerp(c00.position, c10.position, u),
                               lerp(c01.position, c11.position, u), v);
    const float3 normal =
        normalize(lerp(lerp(c00.normal, c10.normal, u), lerp(c01.normal, c11.normal, u), v));
    const float3 root = push.origin.xyz + ground;

    // Thinner further off, down to those standing for cells further up the lattice, each blade
    // going at its own point as the field thins past it and shrinking away as it does, so the
    // field thins without a pop. Thick patches hold their density only as far as it takes them to
    // thin to the rest of the field's, as no one sees far into thick grass: they cost no more
    // blades than it, but grow them all up close.
    const float thick = thickness(root, lighting.ground.xyz);
    const float full = tier.x * thick;
    const float hold = tier.y / sqrt(thick);
    const float held = hold / max(length(root), hold);
    const float lattice_cell = spacing * to_grid;
    const float most = exp2(2.0 * float(depth)) / pow(float(patch_cells) * spacing, 2.0);
    const float kept = min(full * held * held, most);
    const float thinned_up = -0.5 * log2(kept * lattice_cell * lattice_cell);
    const float thinning = saturate((kept_share(stood_up, thinned_up) - thinned_by) * 4.0);
    if (thinning <= 0.0) {
        return false;
    }
    // Those left stand for those gone, as wide as they'd cover together.
    const float stands_for = full / max(kept, 1.0);
    // Near another biome a plant may be one of its, the fewer the further in.
    const uint biome = unit(hash + 10u) < near.blend ? near.blend_biome : near.biome;
    const float4 kind = settings.blade[biome];
    const float4 spread = settings.spread[biome];
    // Where the trees are gone, grass takes the open ground, once the black of a fire has greened.
    const float sown_share =
        lerp(spread.x, max(spread.x, pioneers), near.gone) * (1.0 - near.scar);
    const bool lawn_here = kind.w > 0.0 && unit(hash + 2u) < sown_share;
    if (!lawn_here && spread.w <= 0.0) {
        return false;
    }
    // Sparse biomes grow in clumps rather than an evenly thinned lawn: desert bunchgrass stands
    // in tufts of its own, bare ground between.
    if (spread.z < 1.0 && clumped(blade, spot, seed, spread.z) <= 0.0) {
        return false;
    }

    const float above = height_above_sea(root, push.bend_up, push.bend_radii.x);
    // At the waterline a reed may stand; elsewhere the lawn grows, keeping off the surf.
    const bool reed = spread.w > 0.0 && above >= reed_low && above <= reed_high &&
                      unit(hash + 9u) < spread.w;
    const float tall = reed ? reed_height * (0.75 + (0.45 * unit(hash + 5u)))
                            : kind.w * thick_height(thick) * (1.0 + (0.6 * near.gone)) *
                                  (0.6 + (0.8 * unit(hash + 5u)));
    // Each blade ends at its own point towards the end of the reach, and under its own few pixels
    // tall, so short grass stays near, tall grass reaches further, and the field frays away over a
    // wide band rather than stopping along a line.
    const float distance = length(root);
    const float pixel = push.bend_radii.w * distance;
    const float ends = fade_far * (1.0 - (fraying * unit(hash + 12u)));
    const float least = 1.0 + (2.0 * unit(hash + 13u));
    const float fade = saturate((ends - distance) / (0.1 * fade_far)) *
                       saturate((tall / pixel) - least);
    if (fade <= 0.0 || (!reed && (!lawn_here || above < shore))) {
        return false;
    }

    // Leaning its own slight way, facing its own way.
    const float3 beside = normalize(abs(normal.z) < 0.9 ? cross(normal, float3(0.0, 0.0, 1.0))
                                                        : cross(normal, float3(1.0, 0.0, 0.0)));
    const float3 ahead = cross(normal, beside);
    const float facing = 6.2831853 * unit(hash + 3u);
    const float3 width_dir = (beside * cos(facing)) + (ahead * sin(facing));
    const float leaning = 6.2831853 * unit(hash + 4u);
    // Reeds stand straighter than grass leans.
    const float3 up = normalize(
        normal + ((reed ? 0.05 : 0.2) * ((beside * cos(leaning)) + (ahead * sin(leaning)))));

    const float height = tall * fade * thinning;
    // At least a pixel wide, so far blades don't shatter into bright specks; drawn wider than
    // they are, they wear the ground's colour instead, melting into it.
    const float true_width =
        (reed ? reed_width : spread.y * stands_for) * (0.7 + (0.6 * unit(hash + 6u)));
    const float width = max(true_width, 0.8 * pixel);

    grown.root = root;
    grown.above = above;
    grown.across = width_dir * width;
    grown.rise = up * height;
    // Stems are stiff where blades are carried.
    grown.tip = blown(root, height, hash + 7u) * (reed ? 0.4 : 1.0);
    grown.normal = normal;
    grown.biome = biome;
    grown.reed = reed;
    // Wearing the ground's colour as it goes, so the field fades into the ground beyond.
    const float going = max(smoothstep(fade_far * (1.0 - (2.0 * fraying)), ends, distance),
                            smoothstep(least + 1.5, least, tall / pixel));
    grown.itself = (true_width / width) * (1.0 - going);
    // Darker where it grows thick and shades itself, paler where it's thin.
    const float lushness = saturate((thick - thinnest) / (thickest - thinnest));
    grown.tone = (0.8 + (0.4 * unit(hash + 8u))) * lerp(1.12, 0.85, lushness);
    return true;
}

// Whether any of a blade may be seen, as far as the view's sides and near plane go.
bool in_view(Grown grown) {
    const float3 middle = bent(grown.root + (0.5 * (grown.rise + grown.tip)), push.bend_up,
                               push.bend_radii.xy);
    const float reach = (0.5 * length(grown.rise)) + length(grown.tip) + length(grown.across);
    const float4x4 clip = push.clip_from_eye;
    const float4 planes[5] = { clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1],
                               clip[3] - clip[1], clip[3] - clip[2] };
    for (uint plane = 0; plane < 5; ++plane) {
        const float4 side = planes[plane];
        if (dot(side.xyz, middle) + side.w < -reach * length(side.xyz)) {
            return false;
        }
    }
    return true;
}

// The frame's blades, packed: the root and its height above sea level; then the quad's width,
// rise and tip, the ground's normal, and its kind, as pairs of 16-bit fractions of a span.
struct Packed {
    float4 root;
    uint4 shape;
    uint4 more;
};

// Metres either way the quad's width, rise and tip may reach, as packed.
static const float shape_span = 4.0;

uint pair(float low, float high, float span) {
    const int2 whole = int2(round(clamp(float2(low, high) / span, -1.0, 1.0) * 32767.0));
    return (uint(whole.x) & 0xffffu) | (uint(whole.y) << 16);
}

float low_of(uint both, float span) {
    return float(int(both << 16) >> 16) * (span / 32767.0);
}

float high_of(uint both, float span) {
    return float(int(both) >> 16) * (span / 32767.0);
}

Packed packed(Grown grown) {
    Packed out;
    out.root = float4(grown.root, grown.above);
    out.shape = uint4(pair(grown.across.x, grown.across.y, shape_span),
                      pair(grown.across.z, grown.rise.x, shape_span),
                      pair(grown.rise.y, grown.rise.z, shape_span),
                      pair(grown.tip.x, grown.tip.y, shape_span));
    out.more = uint4(pair(grown.tip.z, 0.0, shape_span), pair(grown.normal.x, grown.normal.y, 1.0),
                     pair(grown.normal.z, grown.itself, 1.0),
                     grown.biome | (grown.reed ? 0x100u : 0u) |
                         (uint(round(saturate(grown.tone / 2.0) * 255.0)) << 16));
    return out;
}

Grown unpacked(Packed from) {
    Grown grown;
    grown.root = from.root.xyz;
    grown.above = from.root.w;
    grown.across = float3(low_of(from.shape.x, shape_span), high_of(from.shape.x, shape_span),
                          low_of(from.shape.y, shape_span));
    grown.rise = float3(high_of(from.shape.y, shape_span), low_of(from.shape.z, shape_span),
                        high_of(from.shape.z, shape_span));
    grown.tip = float3(low_of(from.shape.w, shape_span), high_of(from.shape.w, shape_span),
                       low_of(from.more.x, shape_span));
    grown.normal =
        float3(low_of(from.more.y, 1.0), high_of(from.more.y, 1.0), low_of(from.more.z, 1.0));
    grown.itself = high_of(from.more.z, 1.0);
    grown.tone = float((from.more.w >> 16) & 0xffu) * (2.0 / 255.0);
    grown.biome = min(from.more.w & 0xffu, biome_count - 1);
    grown.reed = (from.more.w & 0x100u) != 0;
    return grown;
}

// The blades grown this frame, as many as it holds, and the draw that draws them: six vertices,
// and as many instances as were grown, counted as they're added, those past what it holds too.
[[vk::binding(7, 0)]] RWStructuredBuffer<Packed> grown_blades;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> grown_draw;
// The same blades, as they're drawn.
[[vk::binding(9, 0)]] StructuredBuffer<Packed> blades;

static const uint group_size = 64;

[shader("compute")]
[numthreads(group_size, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    Grown grown;
    if (!grow(id.x, grown) || !in_view(grown)) {
        return;
    }
    uint at;
    InterlockedAdd(grown_draw[1], 1u, at);
    uint capacity;
    uint stride;
    grown_blades.GetDimensions(capacity, stride);
    if (at < capacity) {
        grown_blades[at] = packed(grown);
    }
}

[shader("vertex")]
Blade vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    uint capacity;
    uint stride;
    blades.GetDimensions(capacity, stride);
    if (instance >= capacity) {
        return nothing();
    }
    const Grown grown = unpacked(blades[instance]);
    const float4 kind = settings.blade[grown.biome];

    // Two triangles tapering to a narrow tip, the tip carried by the wind.
    const uint corner = corners[vertex];
    const float side = (corner & 1u) == 0 ? -0.5 : 0.5;
    const float t = corner < 2 ? 0.0 : 1.0;
    const float taper = grown.reed ? 0.25 : 0.8;
    const float3 placed = grown.root + (grown.across * (side * (1.0 - (taper * t)))) +
                          (grown.rise * t) + (grown.tip * (t * t));

    Blade blade;
    blade.from_eye = placed;
    blade.drawn = bent(placed, push.bend_up, push.bend_radii.xy);
    blade.position = mul(push.clip_from_eye, float4(blade.drawn, 1.0));
    blade.normal = grown.normal;
    blade.height = grown.above;
    // Darker towards the root, as fields shade themselves; the ground's own tone where the blade
    // is really thinner than drawn.
    const float its_own = grown.tone * (0.55 + (0.45 * t));
    const float3 own = grown.reed ? reed_stem : kind.rgb;
    const float3 field = kind.rgb * 1.25 * field_shade;
    blade.colour = lerp(field, own * its_own, grown.itself);
    blade.head = lerp(field, reed_head, grown.itself);
    blade.t_up = t;
    blade.cut = grown.reed ? head_cut : 2.0;
    blade.side = side;
    blade.stem_half = grown.reed ? lerp(0.5, 0.18, grown.itself) : 1.0;
    // Blades bent along the wind catch the light on their flanks, so gusts cross far fields as
    // patches of light even where single blades have melted away.
    const float speed = length(push.wind.xyz);
    const float height = length(grown.rise);
    const float waving =
        speed > 1e-3 && height > 0.0 ? dot(grown.tip, push.wind.xyz) / (speed * height) : 0.0;
    blade.colour *= 1.0 + (waving * lerp(0.15, 0.35, 1.0 - grown.itself));
    return blade;
}

// Lit as the ground beneath it: by sunlight through the air and the sky's light, seen through the
// air between, and tone mapped, as terrain.slang lights the ground.
[shader("fragment")]
float4 fragment_main(Blade blade) : SV_Target {
    // A reed's stem is trimmed from the quad below the cut; above it the head's colour takes
    // over whole, a crisp band.
    const bool headed = blade.t_up > blade.cut;
    if (!headed && abs(blade.side) > blade.stem_half) {
        discard;
    }
    const float3 albedo = headed ? blade.head : blade.colour;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 normal = normalize(blade.normal);
    const float3 up = normalize(blade.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float3 clear =
        transmittance(air, transmittance_table, max(blade.height, 0.0) * scale, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, blade.drawn);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance =
        through_water_at(albedo * lit / pi, ((sunlight * saturate(dot(up, sun))) + sky) / pi,
                         lighting.carried, blade.from_eye, push.bend_up, push.bend_radii.x,
                         sea_lays_water(lighting));

    const float4 clip = mul(push.clip_from_eye, float4(blade.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(blade.drawn) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
