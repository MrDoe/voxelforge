#version 460
#extension GL_GOOGLE_include_directive : require
// Sun-space depth pass for raster-owned micro meshes (Workstream B).
// Renders the POSED mesh from the sun into an orthographic depth target. The
// skinning chain is copied VERBATIM from micro_mesh.vert, so the shadow map sees
// the animal where it is displayed this frame. Default-off (uSunParams.x==0)
// keeps every gate byte-identical.
#include "micro_rig.glsl"

layout(std430, set = 0, binding = 11) readonly buffer MMeshVerts {
    vec4 uMeshVerts[];
};

layout(location = 0) out float vSunDepth;

void main()
{
    const int w = int(pc.b.z + 0.5);
    const vec4 v = uMeshVerts[gl_VertexIndex];
    const uint word = floatBitsToUint(v.w);

    // EXACT skinning copy of micro_mesh.vert.
    int bi[MICRO_MAX_INFLUENCES];
    float bw[MICRO_MAX_INFLUENCES];
    int dominant;
    int nb = microBoneWeights(w, v.xyz, bi, bw, dominant);
    if (nb == 0) {
        const int bone = int(word & 0xFFu) - 1;
        if (bone >= 0) { bi[0] = bone; bw[0] = 1.0; nb = 1; }
    }
    vec3 baked = v.xyz;
    if (nb > 0) {
        baked = vec3(0.0);
        for (int i = 0; i < nb; ++i)
            baked += microRebone(w, v.xyz, bi[i]) * bw[i];
    }
    const vec3 world = microShadeFromBaked(w, baked);

    const vec4 clip = sun.uSunViewProj * vec4(world, 1.0);
    gl_Position = clip;
    vSunDepth = clamp(clip.z / max(clip.w, 1e-6) * 0.5 + 0.5, 0.0, 1.0);
}
