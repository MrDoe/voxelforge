#version 460
// One vertex per surface-voxel splat. Camera basis comes via the same
// push-constant block as the ray-marcher (no matrices needed).

layout(location = 0) out vec3 vColor;

struct Splat {
    vec4 posRadius; // xyz world pos, w radius (m)
    vec4 color;     // rgb
};

layout(set = 0, binding = 0) readonly buffer Splats {
    Splat splats[];
} uSplats;

layout(push_constant) uniform PC {
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 camFwd;
    vec4 a; // tanHalfFov, aspect, extentX, extentY
    vec4 b; // worldSize, maxEncodedDist, voxelSize, frameIdx
} pc;

void main()
{
    Splat s = uSplats.splats[gl_VertexIndex];
    vec3 p = s.posRadius.xyz;
    float radius = s.posRadius.w;
    vColor = s.color.rgb;

    vec3 rel = p - pc.camPos.xyz;
    vec3 view = vec3(dot(rel, pc.camRight.xyz), dot(rel, pc.camUp.xyz),
                     dot(rel, pc.camFwd.xyz));

    if (view.z < 0.05) {
        // behind camera: push outside clip volume
        gl_Position = vec4(0.0, 0.0, 4.0, 1.0);
        gl_PointSize = 1.0;
        return;
    }

    // Vulkan NDC +Y points DOWN in framebuffer space -> negate for camera-up
    vec2 ndc = view.xy / (view.z * pc.a.x * vec2(pc.a.y, 1.0));
    gl_Position = vec4(ndc.x, -ndc.y, 0.5, 1.0);

    // world-radius -> pixel diameter
    float px = 2.0 * radius / (view.z * pc.a.x) * pc.a.w * 0.5;
    gl_PointSize = clamp(px, 1.0, 48.0);
}
