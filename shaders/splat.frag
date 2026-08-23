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
    float alpha = exp(-4.5 * r2);
    outColor = vec4(vColor * alpha, alpha);
}
