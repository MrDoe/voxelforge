// Shared Gaussian-surfel layout and packed metadata helpers.
//
// The CPU Surfel record remains five vec4s (80 B). mat_ao.w stores baked AO
// plus one metadata encoding:
//   terrain: AO
//   water:   AO + 2
//   object:  AO + 8 + 16 * owningLayerId
//
// Layer IDs are 1..254; zero means unowned/live-only geometry. At ID 254 the
// largest encoded value is about 4073, where float32 still resolves AO much
// more finely than one code. Keeping the ID here avoids a sixth 16-byte GPU
// record and makes vertex, cull, tile, tint and debug paths use one contract.
#ifndef VOXELFORGE_COMMON_SURFEL_GLSL
#define VOXELFORGE_COMMON_SURFEL_GLSL

// SplatUBO contract shared by the forward (splat.frag), vertex and tile
// (splat_tile_render.comp) stages - they must never disagree about which
// pixels get sealed.
//
//   uSplat3.x = base-seal alpha threshold (VF_SPLAT_SEAL_ALPHA, DEFAULT 0 =
//               seal every fragment = the long-standing behaviour).
//
// The base pass (PASS_MODE 4) seals the nearest fragment at alpha 1, which is
// not only the surface's colour: it also stands in for every fragment the
// depth band rejects behind it, because nothing occluded is ever drawn. A
// fragment too thin to stand in for anything - a disk rim, an overhang - is
// therefore better left unsealed, letting the band draw it at its own alpha so
// it fades to zero at the clip instead of ending in an opaque step.
//
// The test reads the UN-WINDOWED kernel alpha (alphaKernel), not the windowed
// alpha: the edge window (uSplat2.w) dominates alpha at every radius, so
// testing the windowed value collapses this into a plain radius test and the
// threshold stops being a threshold (measured: 0.65/0.8/0.9 all render the
// same frame).
//
// It is a run-time field rather than a literal because the correct value is a
// property of the scene's coverage, not of these constants - and it cannot be
// reasoned out: the base pass only sees the single nearest fragment at a pixel
// and cannot know a different fragment covers the same pixel. Measured cost of
// enabling it (threshold 0.5): silhouette 1.28x more transparent for interior
// +5.6/255 with 57% of it blue-dominant, i.e. the sky showing through. Note
// this is NOT the sub-centimetre micros - VF_MICRO=0 measures 5.96 vs 5.60.
// A real fix needs a coverage count, which the unused D24_S8 stencil (already
// allocated, bound and cleared) is the natural home for.

struct Surfel {
    vec4 pos_rU;    // xyz = centre (m), w = radiusU (m)
    vec4 normal_rV; // xyz = geometric normal, w = radiusV (m)
    vec4 bent_sh;   // xyz = baked bent normal, w = baked shadow
    vec4 mat_ao;    // xyz = material/reflectivity/roughness, w = packed AO metadata
    vec4 tan_aspect; // xyz = in-plane tangent, w = per-cell texture override
};

bool surfelIsWater(float packedAo)
{
    return packedAo > 1.5 && packedAo < 5.5;
}

bool surfelIsObject(float packedAo)
{
    return packedAo >= 7.5;
}

// bent_sh.w packs TWO baked fields: the smoothed sun shadow (0..1) and the
// exact per-surfel sky verdict (0/1) as  w = shadow + 2*sky. OBJECT surfels
// carry the sky bit (the CPU bake resolves the SVO's own sky ray against the
// records field); terrain and water leave it 0 and keep the fragment test.
// ALWAYS decode before reading a shadow - the packed value reaches 3.
float surfelBakedShadow(float packedSh) { return packedSh - 2.0 * step(1.5, packedSh); }
float surfelBakedSky(float packedSh) { return step(1.5, packedSh); }

uint surfelLayerId(float packedAo)
{
    if (!surfelIsObject(packedAo))
        return 0u;
    // AO is in [0,1], so rounding recovers the integer lane even at the top
    // of the supported layer range where float spacing is ~0.0005.
    return uint(max(0.0, floor((packedAo - 8.0) / 16.0 + 0.5)));
}

float surfelBakedAo(float packedAo)
{
    if (surfelIsWater(packedAo))
        return packedAo - 2.0;
    if (surfelIsObject(packedAo))
        return packedAo - (8.0 + 16.0 * float(surfelLayerId(packedAo)));
    return packedAo;
}

bool surfelBelongsToLayer(float packedAo, uint layerId)
{
    return layerId > 0u && surfelLayerId(packedAo) == layerId;
}

// ---- single-face disk clip ------------------------------------------------
// A disk that stands for EXACTLY ONE exposed voxel face is flagged by the
// bake as mat_ao.x = material + 0.25, and for such a disk the bake
// guarantees that the normal IS the exact lattice axis (+-X/Y/Z) and that the
// position is that face's centre. Such a disk must not claim pixels outside
// the face's square: the radius is 1.4 cells against a 0.1 m face, so the
// disk spills ~4 cm past the face, and at a canopy's density those spills
// fill the gaps between the leaves - the splat path then paints a shadowed
// neighbour where the SVO reference marches the ray BETWEEN the leaves onto
// the lit ground behind (measured: the splat-only near-black pixels carry a
// warm bark/wood albedo where the SVO carries a neutral grey one).
//
// The clip must DISCARD in EVERY pass, including the depth-only prepass. If
// the spilled part still wrote a depth, the true surface behind it would be
// rejected by the band and the base seal would still write the spilled
// colour at alpha 1 - the clip would be a no-op.
bool surfelIsSingleFace(float packedMat)
{
    // The material is an integer, so the fraction is 0 for a plain material
    // and 0.25 for a flagged one. The existing readers (uint(vMat.x + 0.5)
//    and the abs(vMat.x - 8.0) < 0.5 material tests) are unaffected.
    return fract(packedMat) > 0.1;
}

// The face's square: the two lattice axes perpendicular to the face's axis,
// half a voxel either side of the face's centre.
bool surfelInsideFace(vec3 q, vec3 centre, vec3 n, float voxel)
{
    vec3 an = abs(normalize(n));
    vec3 t1, t2;
    if (an.x > 0.5) {
        t1 = vec3(0.0, 1.0, 0.0);
        t2 = vec3(0.0, 0.0, 1.0);
    } else if (an.y > 0.5) {
        t1 = vec3(1.0, 0.0, 0.0);
        t2 = vec3(0.0, 0.0, 1.0);
    } else {
        t1 = vec3(1.0, 0.0, 0.0);
        t2 = vec3(0.0, 1.0, 0.0);
    }
    vec3 d = q - centre;
    float h = 0.5 * voxel;
    return abs(dot(d, t1)) <= h && abs(dot(d, t2)) <= h;
}

// ---- Brush hover preview volume (shared by both backends) ----------------
// The app fills a std140 BrushUBO (binding 13 in BOTH the splat fragment
// pipeline and the SVO compute pipeline) with the active edit-brush volume so
// the geometry a stamp would affect can be seen before the click. The layout
// is byte-identical to SplatPass::setBrush, so one declaration serves both.
//
//   bVolume: xyz = centre (world m), w = radius m
//   bAxis:   xyz = unit axis, w = half length m (0 = ball/box, <0 = dome)
//   bTint:   rgb = tint colour, a = strength (a <= 0 disables the preview)
//   bMeta:   x   = optional owning layer ID; 0 = spatial volume only
//            y   = radial falloff CURVE index (0 = Constant, i.e. no taper)
//   bBoxLo/bBoxHi: the ACTIVE bounding-box selection in INCLUSIVE lattice
//            cell indices (w = 1). A stamp is scoped to the box on the CPU
//            (CellBox::clampEdits), so the tint must clip to the same cells
//            or it would promise material outside the box that the stamp
//            never writes. The test converts the surfel centre to a cell with
//            floor((p + worldSize/2) / voxelSize) - the same integer compare
//            the CPU makes on the edit's lattice coords. `worldSize` rides
//            pc.b.x; it is passed in for the same reason voxelSize is.

// The CPU's EditableWorld::FalloffCurve set, indexed by bMeta.y. Duplicated
// here so the hover tint marks the volume the stamp actually emits; the two
// must agree curve for curve, and test-preview is the gate.
float brushFalloffCurve(int idx, float q)
{
    q = clamp(q, 0.0, 1.0);
    if (idx == 1) return sqrt(max(0.0, 1.0 - q * q));       // Sphere
    if (idx == 2) return sqrt(max(0.0, 1.0 - q));           // Root
    if (idx == 3) return 1.0 - q * q * (3.0 - 2.0 * q);     // Smooth
    if (idx == 4) return 1.0 - q;                           // Linear
    if (idx == 5) return (1.0 - q) * (1.0 - q);             // Sharp
    return 1.0;                                             // Constant
}
//
// w == 0 on the volume with w != 0 on the axis is a BOX (axis.xyz are half
// extents) - the rotate/move trackball preview tints a whole layer AABB.
// `voxelSize` is passed in rather than read from a push constant because this
// header is included before the pipeline's own constant block is declared.
bool sharedInBrushVolume(vec3 p, float packedAo, vec4 bVolume, vec4 bAxis,
                         vec4 bTint, vec4 bMeta, float voxelSize, float worldSize,
                         vec4 bBoxLo, vec4 bBoxHi)
{
    if (bTint.a <= 0.0)
        return false;
    const uint layer = uint(max(0.0, bMeta.x));
    if (layer > 0u && !surfelBelongsToLayer(packedAo, layer))
        return false;
    // Bounding-box selection clip: the stamp only writes cells inside the
    // box, so the tint must not mark the rest of the brush volume. Water is
    // skipped here - the fixed-level plane is skipped at the tint site.
    if (bBoxHi.w > 0.5) {
        ivec3 cell = ivec3(floor((p + 0.5 * worldSize) / voxelSize));
        if (any(lessThan(cell, ivec3(floor(bBoxLo.xyz)))) ||
            any(greaterThan(cell, ivec3(floor(bBoxHi.xyz)))))
            return false;
    }
    vec3 rel = p - bVolume.xyz;
    const float r2 = bVolume.w * bVolume.w;
    if (bVolume.w == 0.0) {
        // box: rel within the half extents on every axis (the skin is
        // folded into the extents by the caller)
        vec3 he = bAxis.xyz;
        return all(lessThanEqual(abs(rel), he));
    }
    if (bAxis.w == 0.0) {
        // ball (Delete/Paint): a tapering curve grades the RADIUS, matching
        // EditableWorld::makeSphere. Constant is the original hard sphere.
        const int fo = int(bMeta.y + 0.5);
        if (fo == 0)
            return dot(rel, rel) <= r2;
        const float d = sqrt(dot(rel, rel));
        const float q = d / max(bVolume.w, 1e-6);
        return d <= bVolume.w * brushFalloffCurve(fo, q);
    }
    const float along = dot(rel, bAxis.xyz);
    const vec3 perp = rel - bAxis.xyz * along;
    if (bAxis.w < 0.0) {
        // dome: the footprint disk (radius r) extruded |w| up the axis, closed
        // by a fillet of radius min(r, |w|) / 2 - exactly the set
        // EditableWorld::makeDome emits. The base layer counts (makeDome's
        // -voxelSize epsilon), and the two profile branches meet at t = lipY.
        const float h = -bAxis.w;
        const float r = bVolume.w;
        if (along < -voxelSize)
            return false;
        // Radial falloff (bMeta.y = curve index): the same profile as
        // EditableWorld::falloffCurveAt, applied per column with the fillet
        // shrinking to match, so the tint marks the tapered growth rather than
        // the full-depth one. Constant (0) grades nothing, which keeps the
        // legacy flat-top footprint exact.
        const int fo = int(bMeta.y + 0.5);
        const float qq = sqrt(dot(perp, perp)) / max(r, 1e-6);
        const float prof = brushFalloffCurve(fo, qq);
        const float hf = h * prof;
        if (hf < 0.5 * voxelSize)
            return dot(perp, perp) <= r2; // tapered to nothing past the base layer
        const float c = 0.5 * min(r, hf);
        const float lipY = hf - c;
        if (along <= lipY)
            return dot(perp, perp) <= r2;
        const float k = along - lipY;
        if (k > c)
            return false;
        const float rr = (r - c) + sqrt(max(0.0, c * c - k * k));
        return dot(perp, perp) <= rr * rr;
    }
    // Carve cylinder. The near end (kCarveTopMargin above the hit) is NOT
    // tapered - it is what opens the ground the scoop starts at - so the far
    // end alone is scaled by the profile, mapping [−w, +w] onto
    // [−w, w·f] as f goes 0 → 1.
    const int fo = int(bMeta.y + 0.5);
    const float qq = sqrt(dot(perp, perp)) / max(bVolume.w, 1e-6);
    const float far = mix(-bAxis.w, bAxis.w, brushFalloffCurve(fo, qq));
    return along >= -bAxis.w && along <= far && dot(perp, perp) <= r2;
}

#endif
