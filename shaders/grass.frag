#version 460
#extension GL_GOOGLE_include_directive : require
// Ground foliage cards, RASTER path (fragment stage).
//
// Samples the CC0 photo card from the shared atlas at the layer bound to the
// ground material (no material id is hardcoded here), depth-tests against the
// G-buffer like fur.frag, and adds the SAME alpha-weighted terms to the SAME
// five accumulation targets the fur composite folds into HDR. No new
// composite pass, no new binding except the card SSBO (31) in the vertex
// stage. A card behind the world or behind water is discarded.

layout(set = 0, binding = 14, rgba16f) uniform readonly highp image2D uOutGPos;
layout(set = 0, binding = 22) uniform sampler2DArray uTexAtlas;

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a;
    vec4 b;
    vec4 sunDir;
    vec4 misc;
} pc;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vN;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in vec4 vCard;  // layer, alpha, material id, tone
layout(location = 4) flat in float vEnergy;

layout(location = 0) out vec4 oA;
layout(location = 1) out vec4 oB;
layout(location = 2) out vec4 oC;
layout(location = 3) out vec4 oD;
layout(location = 4) out vec4 oE;

void main()
{
    // Clamp UVs: the atlas sampler is REPEAT, so edge texels would wrap.
    const vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    const vec4 tex = textureLod(uTexAtlas, vec3(uv, vCard.x), 0.0);
    // pc.misc.z is tonemapLook in the shared push, which neither grass stage
    // reads; FurPass re-pushes just the grass draw with the opacity slider
    // there. Scaling here (rather than in the composite) makes every
    // accumulation term — and so the final 1-exp(-sumA) coverage — fade
    // together, and keeps the sun-shadow tagging in oE.w consistent.
    const float opacity = clamp(pc.misc.z, 0.0f, 1.0f);
    const float alpha = tex.a * vCard.y * vEnergy * opacity;
    if (alpha < 0.01)
        discard;

    // Depth reference: the nearest opaque surface already in the G-buffer.
    const vec2 ndc = gl_FragCoord.xy / vec2(pc.a.z, pc.a.w) * 2.0 - 1.0;
    const vec3 rd = normalize(pc.camFwd.xyz + ndc.x * pc.a.x * pc.a.y * pc.camRight.xyz -
                              ndc.y * pc.a.x * pc.camUp.xyz);
    const vec4 gp = imageLoad(uOutGPos, ivec2(gl_FragCoord.xy));
    const bool hasScene = !(gp.xyz == vec3(0.0));
    const float sceneT = hasScene ? dot(gp.xyz - pc.camPos.xyz, rd) : 1e9;
    if (dot(vWorld - pc.camPos.xyz, rd) > sceneT + 0.02)
        discard;

    vec3 n = normalize(vN);
    if (dot(n, rd) > 0.0)
        n = -n; // blades are two-sided
    const float ndl = max(dot(n, normalize(pc.sunDir.xyz)), 0.0);
    // A grounded blade has a dark root and a lit tip: the composite's sky
    // term handles the rest. Bake a simple gradient into the AO term.
    const float ao = mix(0.55, 1.05, vUV.y);
    // The photo carries its own colour; a per-card tone keeps clumps from
    // looking like clones. Material id feeds the composite's palette lookup.
    const vec3 alb = tex.rgb * (0.82 + 0.36 * vCard.w);

    oA = vec4(alpha, alpha * ndl, alpha * ao, alpha * vCard.z);
    oB = vec4(alpha * vWorld, 0.0);
    oC = vec4(alpha * n, 0.0);
    oD = vec4(alpha * alb, 0.0);
    // xyz is the world position (the composite's rest-position stand-in for
    // cards, which have no skeleton). w carries THIS card's raw sun term
    // (alpha * ndl) so the composite can apply sun occlusion to grass cards
    // only: fur cards leave w = 0 and keep their CPU-baked sun shadow. That
    // is why the two card kinds can share one accumulator set and one
    // composite without the grass shadow double-darkening the fleece.
    oE = vec4(alpha * vWorld, alpha * ndl);
}
