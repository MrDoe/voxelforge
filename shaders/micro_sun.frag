#version 460
#extension GL_GOOGLE_include_directive : require
// Writes sun-space linear depth to the R32F shadow render target. The fragment
// writes a real colour attachment (not an imageStore), so the depth test
// suppresses far fragments deterministically.
layout(location = 0) in float vSunDepth;
layout(location = 0) out float oSunDepth;
void main()
{
    oSunDepth = vSunDepth;
}
