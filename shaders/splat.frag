#version 460
// Isotropic Gaussian sprite, premultiplied alpha.

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0)
        discard;
    // opaque core out to ~55% radius so adjacent splats fuse; soft rim
    float alpha = 1.0 - smoothstep(0.55, 1.0, sqrt(r2));
    // match the raymarch path's gamma-encoded output
    vec3 g = pow(vColor, vec3(1.0 / 2.2));
    outColor = vec4(g * alpha, alpha);
}
