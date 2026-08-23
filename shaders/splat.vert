#version 460
layout(location = 0) out vec3 vAlbedo;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vAO;
layout(location = 3) out float vMat;
layout(location = 4) out vec3 vWorldPos;

struct Splat {
    vec4 posRadius;
    vec4 albedoAO;
    vec4 normalMat;
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
    vNormal = normalize(s.normalMat.xyz);
    vMat = s.normalMat.w;
    vWorldPos = p;

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
    float grow = 1.0 + 0.8 * (1.0 - smoothstep(4.0, 30.0, dCam));
    float px = 2.0 * radius * grow * pc.misc.x / (view.z * pc.a.x) * pc.a.w * 0.5;
    gl_PointSize = clamp(px, 1.0, 512.0);
}
