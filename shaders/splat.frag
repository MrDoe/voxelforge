#version 460
layout(location = 0) in vec3 vAlbedo;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vAO;
layout(location = 3) in float vMat;
layout(location = 4) in vec3 vWorldPos;
layout(location = 0) out vec4 outColor;

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

const vec3 kSunCol = vec3(1.0, 0.95, 0.84) * 1.35;
const vec3 kZenith = vec3(0.20, 0.36, 0.62);
const vec3 kHorizon = vec3(0.72, 0.80, 0.90);
const vec2 kMatRefl[9] = vec2[9](
    vec2(35,235)/255.0, vec2(40,230)/255.0, vec2(55,225)/255.0,
    vec2(130,190)/255.0, vec2(95,150)/255.0, vec2(115,135)/255.0,
    vec2(70,160)/255.0, vec2(60,170)/255.0, vec2(30,235)/255.0
);

vec3 aces(vec3 x){ return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }

void main()
{
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;
    float alpha = 1.0 - smoothstep(0.55, 1.0, sqrt(r2));

    vec3 N = normalize(vNormal);
    vec3 sunDir = normalize(pc.sunDir.xyz);
    float ndl = max(dot(N, sunDir), 0.0);
    float ao = pc.misc.w > 0.5 ? vAO : 1.0;
    int mode = int(pc.misc.z + 0.5); // 0 = BBSplat, 1 = 3DGS

    vec3 amb = mix(kHorizon, kZenith, 0.5) * (N.y * 0.5 + 0.5) * 0.55 * ao;
    vec3 bounce = vec3(0.30, 0.25, 0.18) * max(-N.y, 0.0) * 0.35 * ao;
    vec3 col;

    if (mode == 0) {
        // BBSplat: simple Lambert + ambient + AO, no view-dependent specular
        col = vAlbedo * (kSunCol * ndl * 0.9 + amb + bounce);
    } else {
        // 3DGS: PBR-ish with per-material roughness/reflectivity + specular
        uint mid = uint(vMat + 0.5);
        mid = clamp(mid, 0u, 8u);
        vec2 rr = kMatRefl[mid];
        float rough = rr.y;
        float refl = rr.x;
        vec3 V = normalize(pc.camPos.xyz - vWorldPos);
        vec3 Hh = normalize(sunDir + V);
        float NdotH = max(dot(N, Hh), 0.0);
        float specPow = mix(128.0, 8.0, rough);
        float spec = pow(NdotH, specPow) * refl * 0.7;
        // also modulate diffuse by AO
        col = vAlbedo * (kSunCol * ndl * 0.9 + amb + bounce) + kSunCol * spec * ao;
    }

    col = aces(col);
    col = pow(col, vec3(1.0/2.2));
    outColor = vec4(col * alpha, alpha);
}
