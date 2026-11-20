#version 460
#extension GL_GOOGLE_include_directive : require
// Raster mesh for a micro-voxel object (micro_mesh.vert).
//
// A micro object whose layer names a `meshSource` is drawn from its MESH, not
// from the traced field: the mesh is the ground truth for the silhouette, for
// the deformation and for the depth. The field stays behind as the REST POSE
// the shading samples (material, texture, normal, AO, occlusion), which is why
// every point this shader emits is accompanied by the REST point it came from.
//
// The vertices are SKINNED SMOOTHLY: a vertex is moved by EVERY bone whose
// kernel reaches it (micro_weights.glsl), which is the same kernel the CPU's
// MicroRig::boneInfluences evaluates for the voxelizer. One bone per vertex is
// what tore the animal apart - the upper back is equidistant from the spine
// and the last neck link, so a single vertex went with the neck and its
// neighbours stayed, stretching that triangle 108.8x. A continuous weight
// field cannot split: the measured worst triangle is 4.60x with none past 8x.
//
// The `.skin` side-car's per-vertex bone (v.w) is now only a fallback for a
// mesh with no rig at all; the weights are computed here, not stored, so no
// 11 MB vertex buffer is needed.
//
// No vertex buffer: the whole object is one storage buffer of
// (rest.xyz, packed lane) records, and the draw selects the world with
// `firstVertex` + the push constant. A 54 k-vertex animal is 0.9 MB.

#include "micro_rig.glsl"

// The mesh's vertices, all objects concatenated. w is the PACKED vertex lane
// (bits, not a number - read it with floatBitsToUint):
//
//   bits  0..7   bone + 1        (0 = unskinned; the `.skin` fallback)
//   bits  8..15  material        (255 = "no per-vertex material; look it up")
//   bits 16..23  ambient occlusion (255 = open)
//   bits 24..31  reserved (per-vertex sun shadow, for a posed mover)
//
// The layout is written by `meshVertexWord` (src/voxel/micro_mesh.hpp) and
// must not drift from it: a shader that decodes the bytes in another order
// shades the whole animal with the wrong material.
layout(std430, set = 0, binding = 11) readonly buffer MMeshVerts {
    vec4 uMeshVerts[];
};

// Only the two points: the FRAGMENT recomputes the weights from `vRest`
// (interpolating the rest points is exact for a triangle, and the weight field
// is a function of the rest point), so no influence arrays are carried across
// the rasterizer and no `flat` provoking-vertex approximation is needed.
layout(location = 0) out vec3 vRest;  // the object's REST pose, field frame (m)
layout(location = 1) out vec3 vWorld; // the posed world point (m)
// The APPEARANCE travels per corner: the material is an ID, so it cannot be
// interpolated (flat, provoking vertex - a triangle spanning a material seam
// would otherwise blend two palettes), while the AO is a continuous value and
// is interpolated like any other vertex attribute.
layout(location = 2) flat out uint vWord;
layout(location = 3) out float vAo;
// Workstream E1: the REST face normal, so the fragment can shade without the
// field's gradient. Computed from the triangle's three corners (non-indexed).
layout(location = 4) out vec3 vFaceN;

// Linear depth, in metres. The depth attachment is this pass's own (the main
// backend's depth is not touched): it exists only so the mesh occludes ITSELF,
// and the world/micro boundary stays the distance comparison against gpos that
// the traced path already uses. A linear scale keeps the two trivially in step.
const float kMeshDepthScale = 1.0 / 200.0;

void main()
{
    // pc.b.z carries the world index (b.x/b.y are the world's size - read by
    // the shared lighting model - and b.w is the compute dispatch's rectangle),
    // so one pipeline and one buffer serve every object.
    const int w = int(pc.b.z + 0.5);
    const vec4 v = uMeshVerts[gl_VertexIndex];

    // REST face normal from the triangle's three corners (object frame). The
    // mesh is non-indexed, so corners are consecutive: base = idx - idx%3.
    {
        const int base = gl_VertexIndex - (gl_VertexIndex % 3);
        const vec3 a = uMeshVerts[base].xyz;
        const vec3 b = uMeshVerts[base + 1].xyz;
        const vec3 c = uMeshVerts[base + 2].xyz;
        const vec3 fn = cross(b - a, c - a);
        const float fl = length(fn);
        vFaceN = fl > 1e-12f ? fn / fl : vec3(0.f, 1.f, 0.f);
    }

    // The packed lane. `w` is a float because the record is (rest.xyz, w), so
    // the bytes arrive as a BIT PATTERN; decoding it as a number would read a
    // denormal or a NaN and hand the vertex a garbage material.
    const uint word = floatBitsToUint(v.w);
    vWord = word;
    vAo = float((word >> 16) & 0xFFu) * (1.0 / 255.0);

    // THE SKINNING: a weighted sum of each bone's rigid transform, evaluated
    // at the REST vertex. The weights are the kernel's, not a stored array, so
    // this is 9 distance tests per vertex and nothing to upload.
    int bi[MICRO_MAX_INFLUENCES];
    float bw[MICRO_MAX_INFLUENCES];
    int dominant;
    int nb = microBoneWeights(w, v.xyz, bi, bw, dominant);
    if (nb == 0) {
        // No rig: fall back to the `.skin`'s bone (or to the rest pose). The
        // alpaca has a rig, so this is the vase-with-a-mesh case.
        const int bone = int(word & 0xFFu) - 1;
        if (bone >= 0) { bi[0] = bone; bw[0] = 1.0; nb = 1; }
    }
    vec3 baked = v.xyz;
    if (nb > 0) {
        baked = vec3(0.0);
        for (int i = 0; i < nb; ++i)
            baked += microRebone(w, v.xyz, bi[i]) * bw[i];
    }

    // REST -> BAKED -> RENDERED. The bone transforms are the same rigid ones
    // the field is sampled through (R*p + t, chained through their parents),
    // and microShadeFromBaked is the same forward chain microShadeToBaked
    // inverts: the behaviour, the live rotation preview and the staged Move.
    // Both paths therefore agree on where the surface is, including mid-drag.
    const vec3 world = microShadeFromBaked(w, baked);

    vRest = v.xyz;
    vWorld = world;

    const vec3 d = world - pc.camPos.xyz;
    const float zc = dot(d, pc.camFwd.xyz);
    const float xc = dot(d, pc.camRight.xyz);
    const float yc = dot(d, pc.camUp.xyz);
    const float tanHalf = pc.a.x;
    const float aspect = pc.a.y;
    const float zs = max(zc, 1e-4);
    // The exact inverse of the compute tracer's ray for this pixel, so the
    // mesh lands on the same pixel a trace of the same surface would.
    gl_Position = vec4((xc / zs) / (tanHalf * aspect), -(yc / zs) / tanHalf,
                       zc * kMeshDepthScale, 1.0);
}
