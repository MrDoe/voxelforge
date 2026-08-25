#version 460
// GaussianShader splat vert: flattened anisotropic disks (shortest axis = normal)
// Stride 5: posRadius, albedoAO, normalMat, shadeParams, shadow
layout(location = 0) out vec3 vAlbedo;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vAO;
layout(location = 3) out float vMat;
layout(location = 4) out vec3 vWorldPos;
layout(location = 5) out vec3 vSpecTint;
layout(location = 6) out float vRough;
layout(location = 7) out float vShadow;

struct Splat {
    vec4 posRadius;
    vec4 albedoAO;
    vec4 normalMat;
    vec4 shadeParams; // xyz specularTint, w roughness
    vec4 shadow; // x=shadow 0..1 (minimal raytracing)
};

layout(set = 0, binding = 0) readonly buffer Splats {
    Splat splats[];
} uSplats;

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

void main()
{
    Splat s = uSplats.splats[gl_VertexIndex];
    vec3 p = s.posRadius.xyz;
    float radius = s.posRadius.w;
    vAlbedo = s.albedoAO.rgb;
    vAO = s.albedoAO.a;
    // normal is shortest axis v (GaussianShader §3.3); will be view-flipped in frag
    vNormal = s.normalMat.xyz;
    vMat = s.normalMat.w;
    vSpecTint = s.shadeParams.rgb;
    vRough = s.shadeParams.a;
    vShadow = s.shadow.x;
    vWorldPos = p;

    // view-space
    vec3 rel = p - pc.camPos.xyz;
    vec3 view = vec3(dot(rel, pc.camRight.xyz), dot(rel, pc.camUp.xyz), dot(rel, pc.camFwd.xyz));
    if (view.z < 0.05) {
        gl_Position = vec4(0.0, 0.0, 4.0, 1.0);
        gl_PointSize = 1.0;
        return;
    }
    vec2 ndc = view.xy / (view.z * pc.a.x * vec2(pc.a.y, 1.0));
    gl_Position = vec4(ndc.x, -ndc.y, 0.5, 1.0);
    float dCam = length(rel);
    // GaussianShader flattened: thin axis along N; uniform radius (no far boost per user request)
    // Keep foreshortening for disk orientation but no distance grow
    vec3 N = normalize(vNormal);
    vec3 V = normalize(-rel);
    float ndv = clamp(abs(dot(N, V)), 0.0, 1.0);
    float foreshorten = mix(0.55, 1.0, ndv);
    float grow = 1.0;
    float px = 2.0 * radius * grow * foreshorten * pc.misc.x / (view.z * pc.a.x) * pc.a.w * 0.5;
    gl_PointSize = clamp(px, 1.0, 512.0);
}
