// Shared micro-voxel SHADING model (micro_shade.glsl).
//
// Included by micro_overlay.comp (the compute sphere tracer) and by
// micro_mesh.frag (the raster mesh path), so one micro object is lit by ONE
// model whichever way its geometry reached the G-buffer: the field supplies
// material, texture, normal, AO and occlusion, the path supplies the surface.
//
// The world table, the field layout and the rigid transforms live in
// micro_rig.glsl, which this includes (the vertex stager includes only that).

#include "micro_rig.glsl"

// Where the main backend already resolved this pixel. The overlay composites
// into these rather than owning new targets, so micro geometry lands in the
// same HDR/G-buffer the post chain, SSAO, SSR, fog and bloom all read.
layout(set = 0, binding = 12, rgba16f) uniform highp image2D uOutHdr;
layout(set = 0, binding = 13, rgba16f) uniform highp image2D uOutGPos;
layout(set = 0, binding = 14, rgba16f) uniform highp image2D uOutGNorm;

// Workstream B: the per-frame R32F sun-space depth, copied from the render
// target after the sun pass. Read-only in the shading stages.
layout(set = 0, binding = 29, r32f) uniform readonly highp image2D uSunDepth;

#define VOXELFORGE_MICRO_BACKEND 1
#include "common_base.glsl"
#include "common_irradiance.glsl"

// ---------------------------------------------------------------------------
// Octree traversal over the MICRO worlds.
//
// This is deliberately a SEPARATE copy of the traversal from common_svo.glsl
// rather than a shared one parameterised by a world index. The main shader's
// six world-resolution sites are the world's hit path; parameterising them would
// put the 0.1 m render at risk for a feature that is off by default, and this
// codebase deliberately keeps bit-exact escape hatches (VF_TEXTURES=0,
// VF_RENDER_FLAGS=255) for exactly that reason. Duplicating ~150 lines keeps
// the world path untouched, and with no micro worlds uploaded this shader is
// never dispatched at all.
// ---------------------------------------------------------------------------

// Signed distance to ONE micro world at p, sampled from the DENSE field.
//
// The live ROTATION preview (a micro object dragged round by the trackball)
// is applied HERE, at the one place where the field is sampled: the world
// point is mapped into the object's BAKED frame, the field is read there, and
// the result is a distance in the world frame again (a rotation preserves
// distances, so the value needs no correction and the trace's step stays
// conservative). Everything downstream - the union field, the gradient (and
// so the normal), the AO and the self-shadow - then works in the world frame
// and needs no change at all.
// The dense field is what makes the sphere tracer work. The bricks also carry
// a per-cell distance, but the trilinear stencil reaches one cell past the
// sample point and a brick can only be indexed inside itself, so the stencil
// has to be clamped to the brick - and a clamped stencil makes the field JUMP
// at every brick boundary (measured: +0.5 voxel to -1.0 voxel in one step, with
// no zero crossing between), so the tracer's bisection converges to a
// discontinuity instead of to the surface. The dense array has no such
// boundary: the stencil reads the neighbouring cells of the same array, so the
// field is continuous everywhere the object lives.
//
// Outside the world's AABB it returns the TRUE distance to that box, never a
// flat constant.
//
// This matters more than it looks. The main shader's map() also returns a
// constant (6.0) outside its world, and is safe doing so only because the world
// is 102 m across and the SVO marches with a DDA, not by stepping. A micro
// object is centimetres across and IS sphere-traced, so a 6 m step from the
// camera sails straight past a vase half a metre away and the trace reports a
// miss. Returning the exact box distance makes the outside case a valid
// conservative step instead.
float microMapWorld(int w, vec3 p, float voxel, int gridN, uint gridBase)
{
    // The field lives in the BAKED frame, so the live transform previews map
    // the world point into it first (see microShadeToBaked: the rotation,
    // then the staged Move). The distance that comes back is a distance in the
    // world frame - a translation and a rotation both preserve them.
    //
    // The Move preview belongs HERE, not in main(). It used to shift the hit
    // point after the trace, which meant the TRACE saw the object where it was
    // BAKED while the display moved the surface: the pixels the object covered
    // were its old silhouette, its depth was the old depth, and the shading
    // sampled the object BESIDE the one on screen. As a field transform the
    // trace, the depth test and every surface lookup agree, which is the same
    // rule the behaviour and the rotation preview already follow.
    //
    // A RASTER-OWNED world is the exception: its geometry is a posed mesh, the
    // field under it is the REST POSE, and every query the mesh path makes is
    // already in that frame. Applying the previews or a bone to such a point
    // would deform a point that is not deformed (see microWorldIsMesh).
    if (!microWorldIsMesh(w)) {
        p = microShadeToBaked(w, p);
        // The SKELETON: the field is the REST POSE, so the animated surface is
        // found by sampling through the bone's INVERSE rotation. The bone of a
        // point is the bone whose segment is nearest it.
        p = microUnbone(w, p, microBoneAt(w, p));
    }
    vec3 wmin = wt.uWorlds[w].lo.xyz;
    vec3 wsize = wt.uWorlds[w].hi.xyz - wmin;
    vec3 rp = p - wmin;
    if (any(lessThan(rp, vec3(0.0))) || any(greaterThanEqual(rp, wsize))) {
        vec3 q = max(wmin - p, p - (wmin + wsize));
        float d = length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
        return max(d, 1e-4);
    }
    const ivec3 dim = ivec3(int(wt.uWorlds[w].base.z), int(wt.uWorlds[w].geo.w),
                            int(wt.uWorlds[w].base.w));
    const uint off = uint(wt.uWorlds[w].lo.w);
    // Cell-CENTRE convention: g = c + 0.5 at a cell's centre, so g = c there
    // and the interpolation returns that cell's own value.
    vec3 g = rp / voxel - 0.5;
    ivec3 i0 = ivec3(floor(g));
    vec3 f = g - vec3(i0);
    float s000 = mFieldCell(off, dim, i0 + ivec3(0, 0, 0), voxel);
    float s100 = mFieldCell(off, dim, i0 + ivec3(1, 0, 0), voxel);
    float s010 = mFieldCell(off, dim, i0 + ivec3(0, 1, 0), voxel);
    float s110 = mFieldCell(off, dim, i0 + ivec3(1, 1, 0), voxel);
    float s001 = mFieldCell(off, dim, i0 + ivec3(0, 0, 1), voxel);
    float s101 = mFieldCell(off, dim, i0 + ivec3(1, 0, 1), voxel);
    float s011 = mFieldCell(off, dim, i0 + ivec3(0, 1, 1), voxel);
    float s111 = mFieldCell(off, dim, i0 + ivec3(1, 1, 1), voxel);
    float c00 = mix(s000, s100, f.x), c10 = mix(s010, s110, f.x);
    float c01 = mix(s001, s101, f.x), c11 = mix(s011, s111, f.x);
    float c0 = mix(c00, c10, f.y), c1 = mix(c01, c11, f.y);
    return mix(c0, c1, f.z);
}

float microVoxelSize()
{
    float v = 1e30;
    for (int w = 0; w < wt.uWorldInfo.x && w < MAX_MICRO_WORLDS; ++w)
        v = min(v, wt.uWorlds[w].geo.x);
    return v > 1e29 ? 0.005 : v;
}

// Workstream B: 4-tap PCF in sun space. 1 = lit, 0 = shadowed, matching the
// binary softShadow verdict. The sun depth is the nearest posed-mesh depth
// from the sun; a receiver behind it is shadowed. Disabled -> lit (gates exact).
float sunShadowPCF(vec3 worldPos, vec3 n)
{
    if (sun.uSunParams.x < 0.5)
        return 1.0;
    const vec4 sc = sun.uSunViewProj * vec4(worldPos + n * (2.0 * microVoxelSize()), 1.0);
    const vec2 uv = sc.xy / max(sc.w, 1e-6) * 0.5 + 0.5;
    const float recv = clamp(sc.z / max(sc.w, 1e-6) * 0.5 + 0.5, 0.0, 1.0);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || recv >= 1.0)
        return 1.0; // outside the sun map: no occluder
    const ivec2 sz = imageSize(uSunDepth);
    const vec2 texel = 1.0 / vec2(sz);
    const float bias = 0.0016;
    float vis = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const vec2 o = (vec2(float(i), float(j)) - 0.5) * texel;
            const float d = imageLoad(uSunDepth,
                clamp(ivec2((uv + o) * vec2(sz)), ivec2(0), sz - 1)).r;
            vis += (recv - bias <= d) ? 1.0 : 0.0;
        }
    return vis * 0.25;
}

// Union of every micro world. Because each world contributes a DISTANCE, the
// union is simply the minimum - no nearest-world ordering or bookkeeping is
// needed, and a point in the world's air but inside the vase picks the vase up
// for free. The floor keeps a far-away world's contribution from producing a
// zero step that would livelock the trace.
float microMap(vec3 p)
{
    float d = 1e30;
    for (int w = 0; w < wt.uWorldInfo.x && w < MAX_MICRO_WORLDS; ++w) {
        float voxel = wt.uWorlds[w].geo.x;
        d = min(d, microMapWorld(w, p, voxel, int(wt.uWorlds[w].geo.y),
                                 wt.uWorlds[w].base.x));
    }
    return d;
}

// The union field as SHADING sees it: the SAME field the trace marches.
//
// It used to subtract the staged Move preview here, because main() applied the
// preview as a DISPLAY-only shift after the trace. The preview is part of the
// field-sampling frame now (microMapWorld -> microShadeToBaked), so there is
// nothing left to undo: trace, depth test and every surface lookup agree while
// an object is dragged - the normal no longer collapses to the bounding box's
// and the object no longer shades like cardboard mid-drag. It stays a named
// function because shading must be able to say which field it means.
float microMapShade(vec3 p)
{
    return microMap(p);
}

// Material + surface response of the micro cell that owns the surface hit.
//
// WHICH CELL owns the hit is the whole difficulty, and it is decided by the
// field, not by arithmetic:
//
//  - The trace stops where the signed-distance field crosses zero, which (now
//    that the byte counts eighth-voxels) is exactly ON the object's surface -
//    the face between the outermost SOLID cell and the adjacent AIR cell.
//  - The cell CONTAINING that face is the solid one. The formula here is
//    floor((p - mn) / cellSz) - the cell p is in. It is NOT the
//    trunc((p - mn) / cellSz - 0.5) used by the field stencil, which is the
//    "p sits at a cell CENTRE" convention: at a face that lands on the AIR
//    side, which is what used to paint the vase with the air filler's black
//    albedo and grass material.
//  - The face can coincide with a BRICK boundary, and the octree descent
//    resolves a boundary with ">=", which picks the higher-coordinate side -
//    the wrong side half the time. So the lookup is done a hair INSIDE the
//    solid, along the ray: rd points into the object (the trace hit the front
//    surface), so p + rd * 0.15 voxel is unambiguously on the solid side, in
//    the solid brick, in the solid cell.
//
// If the containing cell still is not solid (a diagonal surface, where the
// zero crossing falls on a cell CORNER rather than a face), the 3x3x3
// neighbourhood is searched and the cell whose centre is nearest the surface
// - the smallest |field| among the negative ones - wins. That is unambiguous
// now that the field is a faithful distance instead of a flat zero band.
//
// Micro cells all carry the object flag (bit 7 of the material byte, set by
// fillMicroBrick), so this never has to consult the terrain heightfield the
// way the main shader's isObjectSurface does - which also sidesteps that
// test's world-scale +0.02 epsilon, four whole cells wide at a 5 mm voxel.
// The field is read in the BAKED frame, so the material lookup - the octree
// descent and the cell that owns the surface - has to happen there too.
// The APPEARANCE of ONE micro world at a point in its REST frame: albedo,
// reflection/roughness and material id, read from the brick cell just inside
// the surface along `restDir` (a direction in the same frame, pointing INTO
// the solid).
//
// Shared by the tracer's hit lookup and by the raster mesh path, which has the
// rest-pose point exactly ON the surface and steps in along the rest normal.
// One lookup, so a traced vase and a rasterised animal cannot disagree about
// which cell owns the surface.
bool microCellAppearance(int w, vec3 rest, vec3 restDir, out vec3 albedo,
                         out vec2 rr, out uint matId)
{
    albedo = vec3(0.5);
    rr = vec2(0.15, 0.9);
    matId = 0u;
    vec3 wmin = wt.uWorlds[w].lo.xyz;
    vec3 wsize = wt.uWorlds[w].hi.xyz - wmin;
    float voxel = wt.uWorlds[w].geo.x;
    vec3 q = rest + restDir * (0.15 * voxel);
    // Clamp the query INTO the box rather than rejecting points outside it.
    // The traced hit lies exactly ON the object's boundary by construction
    // (the trace stops where the field crosses zero, and the field is zero
    // at the AABB face), so a strict inside test rejects the surface it just
    // found and the object renders as the material-lookup-failed colour.
    // Clamping is also what the chunk lookup does one line below, so the two
    // agree at the seam.
    vec3 rp = clamp(q - wmin, vec3(0.0), wsize);
    float chunkM = wt.uWorlds[w].geo.z;
    int gridN = int(wt.uWorlds[w].geo.y);
    ivec3 cc = clamp(ivec3(floor(rp / chunkM)), ivec3(0), ivec3(gridN - 1));
    int ci = int(wt.uWorlds[w].base.y) +
             (cc.z * gridN + cc.y) * gridN + cc.x;
    int root = uMGrid[int(wt.uWorlds[w].base.x) + (cc.z * gridN + cc.y) * gridN +
                        cc.x];
    if (root == -1)
        return false;
    MBases bs = mChunkBases(ci);
    uint h = uint(root);
    vec3 mn = wmin + vec3(cc) * chunkM;
    vec3 sz = vec3(chunkM);
    for (int guard = 0; guard < 12; ++guard) {
        if ((h & 3u) == 1u) {
            uint bi = h >> 2;
            float cellSz = sz.x / float(BRICK_N);
            // The cell CONTAINING the query point.
            ivec3 c0 = ivec3(clamp(floor((q - mn) / cellSz), vec3(0.0),
                                   vec3(float(BRICK_N) - 1.0)));
            ivec3 best = ivec3(-1);
            float bestAbs = 1e30;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        ivec3 c = clamp(c0 + ivec3(dx, dy, dz), ivec3(0),
                                        ivec3(int(BRICK_N) - 1));
                        // The field AT the cell centre. Negative means the
                        // cell is on the solid side of the surface.
                        float fd = mBrickVoxelSdf(bi, bs.brick, c, voxel);
                        if (fd >= 0.0)
                            continue; // air side
                        float a = abs(fd);
                        if (a < bestAbs) {
                            bestAbs = a;
                            best = c;
                        }
                    }
            if (best.x < 0)
                return false; // no solid cell nearby: not a micro surface
            uint vi = (bs.brick + bi) * BRICK_WORDS +
                      (uint(best.z) * BRICK_N * BRICK_N + uint(best.y) * BRICK_N +
                       uint(best.x)) * 2;
            uint w0 = uMBricks[vi];
            uint w1 = uMBricks[vi + 1];
            albedo = vec3(float(w0 & 255u), float((w0 >> 8) & 255u),
                          float((w0 >> 16) & 255u)) / 255.0;
            rr = vec2(float((w1 >> 8) & 255u), float((w1 >> 16) & 255u)) / 255.0;
            matId = (w1 >> 24) & 127u;
            return true;
        }
        if ((h & 3u) != 0u)
            return false;
        uint ni = h >> 2;
        vec3 halfS = sz * 0.5;
        vec3 r2 = q - mn;
        int oct = (r2.x >= halfS.x ? 1 : 0) | (r2.y >= halfS.y ? 2 : 0) |
                  (r2.z >= halfS.z ? 4 : 0);
        h = uMHandles[bs.child + uMChildBase[bs.node + ni] + uint(oct)];
        mn += vec3(oct & 1, (oct >> 1) & 1, (oct >> 2) & 1) * halfS;
        sz = halfS;
    }
    return false;
}

// The material of whichever micro world owns a TRACED hit point.
bool microSurface(vec3 p, vec3 rd, out vec3 albedo, out vec2 rr, out uint matId)
{
    albedo = vec3(0.5);
    rr = vec2(0.15, 0.9);
    matId = 0u;
    for (int w = 0; w < wt.uWorldInfo.x && w < MAX_MICRO_WORLDS; ++w) {
        if (microWorldIsMesh(w))
            continue; // no traced hit can belong to a raster-owned world
        // A hair inside the solid, along the ray - in the BAKED frame, where
        // the ray direction is the world one turned by the rotation. The
        // bone matters here too: the material lookup is a lookup in the
        // REST POSE's grid, so the point has to be unboned as well.
        vec3 rest = microShadeToBaked(w, p);
        rest = microUnbone(w, rest, microBoneAt(w, rest));
        if (microCellAppearance(w, rest, microDirToBaked(w, rd), albedo, rr, matId))
            return true;
    }
    return false;
}

// Nearest micro hit along a ray, or false. Sphere-traces the union field then
// bisects the final interval, which is robust against the brick field's
// quantised (and therefore not perfectly smooth) distance.
//
// Step sizing is bounded on BOTH sides. The 0.85 factor keeps a quantised field
// from overshooting, and the clamp stops one huge distance (a ray starting far
// from a small object) from consuming the whole trace in a single stride.
bool microTrace(vec3 ro, vec3 rd, float tMax, out float tHit, out int outWorld)
{
    tHit = 0.0;
    outWorld = -1;
    // Each micro world is marched SEPARATELY, so the hit can name the world it
    // belongs to. That matters for the live preview: a staged Move offset
    // applies to ONE world, and the shader has to know which world the surface
    // it is shading belongs to. (The union field is still what the SHADOW
    // march uses - a shadow does not care which world it belongs to.)
    float bestT = tMax;
    int bestW = -1;
    for (int w = 0; w < wt.uWorldInfo.x && w < MAX_MICRO_WORLDS; ++w) {
        // A raster-owned world is drawn by the mesh pass, which owns its
        // silhouette, depth and normals. Tracing its rest field here would
        // draw the REST POSE on top of the posed mesh - the object would be
        // its own ghost. The field still serves AO/shadow/gradient, which is
        // why this skip lives in the TRACE and not in the field sampler.
        if (microWorldIsMesh(w))
            continue;
        const float voxel = wt.uWorlds[w].geo.x;
        const int gridN = int(wt.uWorlds[w].geo.y);
        const uint gridBase = wt.uWorlds[w].base.x;
        // The minimum step is a QUARTER OF A MICRO VOXEL, not a fixed epsilon.
        // It has to be able to carry the ray ACROSS the surface: a sphere
        // tracer steps by a fraction of the distance, so it approaches the
        // surface geometrically and never actually reaches it - without a
        // floor big enough to overshoot, the sample never lands inside the
        // object and the hit is never detected.
        const float minStep = 0.25 * voxel;
        if (microMapWorld(w, ro, voxel, gridN, gridBase) < 0.0)
            continue; // the camera is inside this world; it cannot be seen
        float t = minStep;
        float prevT = t;
        for (int i = 0; i < 128; ++i) {
            if (t > bestT)
                break;
            const float d = microMapWorld(w, ro + rd * t, voxel, gridN, gridBase);
            // STRICTLY INSIDE, not "within eps of the surface". Outside the
            // world's AABB the field falls back to the distance to the box,
            // which is ZERO on the box face - so a "d < eps" test fires on the
            // BOX, and the object renders as its own bounding box. The field is
            // negative only inside the object, so "d < 0" is the hit.
            if (d < 0.0) {
                // bisect between the last outside sample and this inside one
                float lo = prevT, hi = t;
                for (int b = 0; b < 8; ++b) {
                    const float mid = 0.5 * (lo + hi);
                    if (microMapWorld(w, ro + rd * mid, voxel, gridN, gridBase) > 0.0)
                        lo = mid;
                    else
                        hi = mid;
                }
                // DISCONTINUITY GUARD. The field is the minimum over the
                // claims of RIGID bone transforms, so it can JUMP from
                // positive to several voxels negative with no zero crossing
                // between (a point whose bone territory flips is mapped to a
                // different place in rest space). The bisection above then
                // converges onto the JUMP, not onto a surface: the octree
                // descent lands in a solid terminal with no brick and the
                // pixel shades black. A genuine crossing lands within a
                // fraction of a voxel of zero; a jump reads at least a voxel
                // deep. Reject it and march on, stepping by the depth the
                // field claims we are inside so the next sample is outside
                // that claimed solid.
                const float dfs = microMapWorld(w, ro + rd * hi, voxel, gridN, gridBase);
                if (dfs < -0.5 * voxel) {
                    prevT = hi;
                    t = hi + clamp(-dfs * 0.85, minStep, 0.25);
                    continue;
                }
                if (hi < bestT) {
                    bestT = hi;
                    bestW = w;
                }
                break;
            }
            prevT = t;
            t += clamp(d * 0.85, minStep, 0.25);
        }
    }
    if (bestW < 0)
        return false;
    tHit = bestT;
    outWorld = bestW;
    return true;
}

// Shadow/light visibility across the micro worlds. NOTE the scope limit: these
// marches resolve the MICRO worlds only, so a micro object is shadowed by its
// own geometry and by other micro objects, but neither by nor onto the main
// world. Making shadows cross between the two would mean giving one side access
// to the other's buffers, which is exactly the separation this pass keeps.
float microVisibility(vec3 ro, vec3 rd, float maxDist)
{
    // ESCAPE THE SURFACE FIRST. The traced hit point sits fractionally INSIDE
    // the solid (the bisection keeps the inside sample), so a visibility ray
    // started there reads d < 0 on its very first step and returns "fully
    // occluded" for every fragment - the object then renders uniformly black,
    // because direct sun is multiplied by this term. March forward until the
    // field is positive before testing anything, and give up as unshadowed if
    // the ray cannot get out (deep inside the object there is nothing to cast).
    const float vs = microVoxelSize();
    float t = 0.0;
    bool escaped = false;
    for (int i = 0; i < 32; ++i) {
        if (microMap(ro + rd * t) > 0.0) { escaped = true; break; }
        t += 0.25 * vs;
    }
    if (!escaped)
        return 1.0;
    for (int i = 0; i < 96; ++i) {
        float d = microMap(ro + rd * t);
        if (d < 0.0)
            return 0.0;
        t += max(d * 0.85, 0.25 * vs);
        if (t > maxDist)
            return 1.0;
    }
    return 1.0;
}

// The MAIN WORLD's coarse shadow, as seen by a micro surface.
//
// The micro pass deliberately has no access to the world's SVO buffers, but it
// does bind the two COARSE world representations every backend already shares:
// uHeight (the 2048^2 terrain heightfield) and uObjVol (the 256^3 r8_snorm
// object volume). That is enough for the shadow a metre-scale object needs
// from a 10 cm world - the same coarse march the splat backend's water path
// uses (softShadowSplat), which is why the two agree.
//
// This is the RECEIVE direction only: a micro object is shadowed by the world.
// The world being shadowed BY a micro object is a separate problem - the
// world's sun shadow is baked per surfel from the VoxelField, and the micro
// objects are not in it. See App::shadeWorldWithMicroShadows.
float worldShadow(vec3 ro, vec3 rd)
{
    if ((gRenderFlags & 2) == 0)
        return 1.0;
    // BINARY, like the SVO reference backend's exact hits and like the micro
    // worlds' own microVisibility. NOT the 0.5-per-unoccluded-tap placeholder
    // that softShadowSplat carries for the water: that returns 0.5 for a fully
    // LIT point, which would halve the sun on every micro surface (measured:
    // the vase's mean luma 109 -> 84 with no occluder anywhere near it).
    // 40 taps with a HARD 0.35 m step cap. The cap is the point: stepping by
    // the distance to the heightfield is cheap, but beyond ~2 m the far-field
    // distance saturates at the object volume's 1.26 m range and the step
    // becomes ~1.07 m - wide enough to step clean OVER a canopy or a roof.
    // The symptom was an alpaca whose body read shadowed and whose LEGS read
    // sunlit (ratio 0.87 vs 0.50): the legs' ray runs lower, so the coarse
    // sample lattice straddled the occluder. The world's own baked shadow uses
    // a fine DDA, which is why the ground under it disagreed with the legs.
    float t = 0.05;
    for (int i = 0; i < 40; ++i) {
        vec3 sp = ro + rd * t;
        float sHf = sp.y - heightAt(sp.xz);
        if (sHf < -0.03)
            return 0.0; // below the terrain
        float sObj = objDist(sp);
        if (sObj < 1.2474 && sObj < -0.05)
            return 0.0; // inside the coarse object volume
        t += clamp(max(min(sHf, sObj), pc.b.y * 0.35) * 0.85, 0.05, 0.35);
        if (t > 20.0)
            break;
    }
    return 1.0;
}

// common_base.glsl forward-declares these; the micro backend supplies its own.
// softShadow is the MICRO worlds' own march (self-shadow and micro-to-micro);
// worldShadow is the coarse world. A micro surface multiplies both.
float softShadow(vec3 ro, vec3 rd)
{
    return microVisibility(ro, rd, 24.0);
}
float lightVisibilitySvo(vec3 ro, vec3 rd, float maxDist)
{
    return microVisibility(ro, normalize(rd), maxDist);
}
float skyVisibilitySvo(vec3 p, vec3 bent)
{
    vec3 dir = normalize(mix(bent, vec3(0.0, 1.0, 0.0), 0.65));
    return microVisibility(p + dir * (3.0 * microVoxelSize()), dir,
                           64.0 * microVoxelSize()) > 0.5 ? 1.0 : 0.05;
}
// applyFlora is already defined by common_base.glsl (it only dispatches to the
// per-backend shadow march), so the micro backend inherits the flora overlay for
// free - a green micro object still gets its grass-blade treatment. Only the
// marches are backend-specific, and those are defined above.

vec3 microNormal(vec3 p, float e)
{
    vec3 g = vec3(microMapShade(p + vec3(e, 0, 0)) - microMapShade(p - vec3(e, 0, 0)),
                  microMapShade(p + vec3(0, e, 0)) - microMapShade(p - vec3(0, e, 0)),
                  microMapShade(p + vec3(0, 0, e)) - microMapShade(p - vec3(0, 0, e)));
    float gl = length(g);
    if (gl < 1e-9)
        return vec3(0.0, 1.0, 0.0);
    return normalize(g);
}

// Multi-scale AO over the micro union, mirroring the SVO backend's sdfAO rings.
// Radii stay in METRES, so they are absolute rather than per-voxel: at a 5 mm
// voxel a voxel-relative first ring would sample inside a single cell and read
// as flat.
void microAO(vec3 p, vec3 n, out float ao, out vec3 bent)
{
    const float rad[3] = float[3](0.006, 0.018, 0.05);
    vec3 tv = cross(n, vec3(0.0001, 1.0, 0.0001));
    vec3 tang = normalize(tv);
    vec3 bitan = normalize(cross(n, tang));
    float occ = 0.0, wsum = 0.0;
    vec3 bd = n * 0.5;
    for (int ring = 0; ring < 3; ++ring) {
        float r = rad[ring];
        for (int a = 0; a < 4; ++a) {
            float ang = float(a) * 1.5708 + float(ring) * 0.785;
            vec3 dir = normalize(n * 0.85 + (cos(ang) * tang + sin(ang) * bitan) * 0.7);
            float s = microMapShade(p + dir * r);
            float fall = clamp(1.0 - s / r, 0.0, 1.0);
            fall = fall * fall * (3.0 - 2.0 * fall);
            float w = 1.0 / (1.0 + fall * fall * 4.0);
            occ += w * fall;
            wsum += w;
            bd += dir * w * (1.0 - fall);
        }
    }
    ao = clamp(1.0 - 0.85 * occ / max(wsum, 1e-4), 0.0, 1.0);
    bent = normalize(bd);
}

// The LIGHTING of a micro surface, from what the path already resolved: the
// visible world point `p`, the point the FIELD's occlusion queries are made at
// (`occlP` - the rest point for a rasterised mesh, `p` for a traced one), the
// world shading normal (already detail-bumped by the caller, which is where the
// rest-frame work lives), the view ray and the surface appearance.
//
// It is shared verbatim by both paths, so a rasterised animal and a traced vase
// cannot drift apart in how they are lit.
// `aoBaked`: Workstream E1 - when >= 0, use this per-vertex baked AO and derive
// enclosure from it (no field march). Traced callers pass -1 (field path).
vec3 shadeMicroLit(int w, vec3 p, vec3 occlP, vec3 n, vec3 rd, vec3 alb, vec2 rr,
                   uint mId, float aoBaked)
{
    const float vs = microVoxelSize();
    // The sun is occluded by BOTH worlds: the micro worlds (self-shadow and
    // micro-to-micro) and the main world's coarse heightfield + object volume.
    // Two binary contributions multiplied is the same verdict the splat
    // backend gives a water fragment, so a vase standing in the house's shadow
    // reads exactly as dark as the ground it stands on.
    float sh = 1.0;
    if ((gRenderFlags & 2) != 0) {
        // Workstream B: raster-owned meshes use the per-frame sun-space PCF map
        // (the posed mesh IS in it); traced worlds keep the field march. Both
        // keep the coarse WORLD shadow, which the micro map does not contain.
        const float microSh = microWorldIsMesh(w)
                                  ? sunShadowPCF(p, n)
                                  : softShadow(occlP + n * (1.0 * vs), kSunDir);
        sh = microSh * worldShadow(p + n * (1.0 * vs), kSunDir);
    }

    float ao = 1.0;
    vec3 bent = n;
    if (aoBaked >= 0.0) {
        // Workstream E1: field-free - use the per-vertex baked AO. bent = n.
        ao = aoBaked;
    } else if ((gRenderFlags & 1) != 0)
        microAO(occlP, n, ao, bent);

    float enclosed = (aoBaked >= 0.0)
        ? (1.0 - ao) // field-free enclosure proxy from AO
        : 1.0 - ((gRenderFlags & 256) != 0 ? skyVisibilitySvo(p, bent) : 1.0);
    enclosed = max(enclosed, aoShEnclosure(ao, sh));
    float skyAmb = mix(1.0, 0.16, enclosed);
    float skyIbl = mix(1.0, 0.04, enclosed);

    float ndl = max(dot(n, kSunDir), 0.0);
    vec3 amb = skyIrradiance(bent) * (0.28 + 0.30 * ao) * skyAmb;
    amb += moonLight(n, ao) * skyAmb;
    vec3 irr = irradianceVolume(p);
    float fold = clamp(-n.y, 0.0, 1.0);
    vec3 bounce = any(greaterThan(irr, vec3(0.0)))
                      ? alb * irr * (0.3 + 0.7 * ao) * skyAmb
                      : (alb * 0.65 + vec3(0.10, 0.09, 0.07)) * fold *
                            (0.3 + 0.7 * ao) * skyAmb;
    vec3 V = -rd;
    vec3 hv = normalize(kSunDir + V);
    float vdh = max(dot(V, hv), 0.0);
    float rough = clamp(rr.y, 0.05, 1.0);
    float f0b = 0.04 + rr.x * 0.70;
    vec3 F = fresnelSchlick(vec3(f0b), vdh);
    float fAvg = clamp((F.r + F.g + F.b) / 3.0, 0.0, 1.0);
    vec3 spec = pbrSpec(n, V, kSunDir, vec3(f0b), rough);
    vec3 ibl = iblContribution(n, V, vec3(f0b), rough) * (0.28 + 0.30 * ao) * skyIbl;
    vec3 col = alb * (1.0 - fAvg) * (sunCol() * ndl * sh + amb + bounce + ibl) +
               spec * sunCol() * ndl * sh +
               spec * alb * 0.30 * sunCol() * sh;
    col += alb * vec3(0.075, 0.080, 0.090) * enclosed * (0.35 + 0.65 * ao);
    if (p.y < kWaterLevel && underWater(p.xz) && !gUnderwater) {
        float depth = kWaterLevel - p.y;
        col *= exp(-depth * vec3(0.35, 0.18, 0.12) * 3.0);
        col = mix(col, vec3(0.05, 0.14, 0.13), clamp(depth * 0.8, 0.0, 0.85));
        col += sunCol() * causticAt(p.xz, pc.misc.y) * ndl * sh * 0.25 *
               exp(-depth * 0.55);
    }
    col += applyLights(p, n, V, rr, alb);
    col += emissiveTerm(p, n, mId, alb);
    return col;
}

// Lit colour for a TRACED micro hit, using the SAME shared lighting terms the
// other backends use (so a vase is not secretly lit by a private model).
vec3 shadeMicro(vec3 p, vec3 rd, int w)
{
    vec3 alb;
    vec2 rr;
    uint mId;
    if (!microSurface(p, rd, alb, rr, mId))
        return vec3(0.0);
    vec3 n = microNormal(p, 1.0 * microVoxelSize());
    // The TEXTURE is sampled in the object's own frame (the rest pose), NOT
    // in the world: a world-space sample slides over the animal as it walks
    // and as its bones swing, because the texture is bound to the surface and
    // not to the world. The rest-pose point is the very point the field is
    // sampled at, so the texture sticks to the surface it belongs to.
    //
    // The NORMAL has to be taken into the rest frame as well. detailAlbedo
    // and detailNormal choose the triplanar projection axes from it, so
    // handing them the ANIMATED normal turns the fur with every bone rotation
    // - the pattern crawls over the skin while the animal moves. The bumped
    // normal is brought back out again for shading.
    const vec3 baked = microShadeToBaked(w, p);
    const int rbone = microBoneAt(w, baked);
    const vec3 rest = microUnbone(w, baked, rbone);
    const vec3 restN = microDirUnbone(w, microDirToBaked(w, n), rbone);
    alb = detailAlbedo(alb, rest, restN, mId);
    n = microDirFromBaked(w, microDirRebone(w, detailNormal(restN, rest, mId), rbone));
    return shadeMicroLit(w, p, p, n, rd, alb, rr, mId, -1.0);
}
