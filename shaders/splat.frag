#version 460
// GaussianShader splat fragment: Eq.3 c = gamma(c_d + s⊙Ls + cr) with HDR env prefilter + GGX
layout(location = 0) in vec3 vAlbedo;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vAO;
layout(location = 3) in float vMat;
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) in vec3 vSpecTint;
layout(location = 6) in float vRough;
layout(location = 7) in float vShadow;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 1) uniform samplerCube uEnv;
layout(push_constant) uniform PC {
    vec4 camPos; vec4 camRight; vec4 camUp; vec4 camFwd; vec4 a; vec4 b; vec4 sunDir; vec4 misc;
} pc;

vec3 aces(vec3 x){ return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0); }

void main(){
    vec2 d = gl_PointCoord*2.0-1.0; float r2=dot(d,d); if(r2>1.0) discard;
    float alpha=1.0 - smoothstep(0.55,1.0, sqrt(r2));
    // Use dithered edge but premultiplied

    vec3 Nraw = normalize(vNormal);
    vec3 V = normalize(pc.camPos.xyz - vWorldPos);
    // Eq.5 view-dependent flip (shortest axis ambiguity) with Δn=0
    vec3 N = Nraw;
    if(dot(N, V) < 0.0) N = -N;
    // water gets slightly perturbed normal for ripple if mat ~ water (we don't have water mat, use world pos y)
    // keep N as is

    vec3 sunDir = normalize(pc.sunDir.xyz);
    float NdotL = max(dot(N, sunDir), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // ambient similar to raymarch
    const vec3 kSunCol = vec3(1.00,0.95,0.84)*1.35;
    const vec3 kZenith = vec3(0.20,0.36,0.62);
    const vec3 kHorizon = vec3(0.72,0.80,0.90);
    vec3 ambient = mix(kHorizon*1.05, kZenith*0.95, clamp(N.y*0.5+0.5,0.0,1.0))*0.55;
    vec3 bounce = vec3(0.30,0.25,0.18)*max(-N.y,0.0)*0.35;
    float ao = pc.misc.w > 0.5 ? vAO : 1.0;

    float shadingMode = pc.misc.z; // 0 BBSplats, 1 GaussianShader
    vec3 col;

    // minimal-raytracing shadow factor (0 lit, 1 shadowed)
    float shadow = clamp(vShadow, 0.0, 1.0);
    // soft shadow darkening: color * (1 - s*0.65)  (implementation_plan.md Step 4)
    float shadowDark = 1.0 - shadow * 0.65;

    if(shadingMode > 0.5){
        // GaussianShader path: diffuse + specular Env (Eq.3, cr=0) with shadow
        vec3 diffuse = vAlbedo * (kSunCol * NdotL * shadowDark + ambient * mix(1.0, 0.7, shadow) + bounce) * ao;

        // Specular via prefiltered HDR env (Eq.4) - also shadowed
        vec3 R = reflect(-V, N);
        float maxMip = 6.0;
        float lod = clamp(vRough * maxMip, 0.0, maxMip);
        vec3 Ls = textureLod(uEnv, R, lod).rgb;
        vec3 F0 = clamp(vSpecTint, vec3(0.0), vec3(1.0));
        vec3 F = F0 + (vec3(1.0)-F0)*pow(1.0 - NdotV, 5.0);
        vec3 specularEnv = Ls * F * 0.7 * shadowDark;

        float rough = clamp(vRough, 0.02, 1.0);
        float specPow = mix(128.0, 8.0, rough);
        vec3 H = normalize(sunDir + V);
        float NdotH = max(dot(N,H),0.0);
        float reflAvg = (F0.r+F0.g+F0.b)/3.0;
        float spec = pow(NdotH, specPow) * reflAvg * 1.1 * shadowDark;
        bool isWater = vWorldPos.y > -1.1 && vWorldPos.y < -0.7 && N.y > 0.9;
        if(isWater){
            float fres = 0.02 + 0.98*pow(1.0 - NdotV, 5.0);
            spec *= (2.0 + fres*3.0);
            diffuse *= 0.35;
            specularEnv *= 1.6;
        }
        col = diffuse + specularEnv + kSunCol * spec;
    } else {
        // BBSplats legacy: simple Lambert + ambient + shadow
        vec3 diffuse = vAlbedo * (kSunCol * NdotL * shadowDark + ambient * mix(1.0, 0.7, shadow) + bounce) * ao;
        float rough = clamp(vRough, 0.02, 1.0);
        float specPow = mix(64.0, 8.0, rough);
        vec3 H = normalize(sunDir + V);
        float NdotH = max(dot(N,H),0.0);
        float spec = pow(NdotH, specPow) * 0.4 * (1.0 - rough) * shadowDark;
        col = diffuse + kSunCol * spec;
    }

    // distance fog (light, matches raymarch 0.0012)
    // we don't have t, approximate fog by distance from cam
    float dist = length(vWorldPos - pc.camPos.xyz);
    float fog = 1.0 - exp(-max(dist - 10.0, 0.0) * 0.0012);
    col = mix(col, kHorizon*1.05, fog*0.7);

    // tonemap
    col = aces(col);
    col = clamp(mix(vec3(dot(col, vec3(0.2126,0.7152,0.0722))), col, 1.55), vec3(0.0), vec3(1.0));
    col = pow(col, vec3(1.0/2.2));
    outColor = vec4(col * alpha, alpha);
}
