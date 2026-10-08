// Coarse irradiance volume - the indirect term that replaces the old
// albedo-tinted stand-in, so a room lit by an actual emitter is tinted by that
// emitter instead of by nothing.
//
// Self-contained by design: the sampler, the world->volume mapping and the
// accessors all live here, so including it in common_base.glsl costs ONE line
// and this stays editable without touching a shared shader file.
//
// WHAT IT IS NOT: this is NOT global illumination. It is DIRECT irradiance from
// the light set, accumulated per cell WITH visibility, plus a sky-presence
// term. Light does not bounce: a hearth lights the air and walls around it, not
// the far side of the room by way of the floor. Calling this "GI" would
// overclaim it. See src/voxel/irradiance_volume.hpp.
//
// WHY A VOLUME: the splat fragment shader already runs twice per covered pixel
// (opaque base pass at depth EQUAL, then the blended band pass) and one
// per-fragment occlusion march is already up to 32 dependent taps. Indirect
// light is low-frequency by nature, so it is the one lighting term worth
// paying a texture fetch for rather than a march.

#ifndef VF_COMMON_IRRADIANCE_GLSL
#define VF_COMMON_IRRADIANCE_GLSL

// Binding 26: verified unused when this was added (the live set was 0-15 and
// 20-25). 25 is the light UBO, 24 the tile rotate UBO, 22/23 the texture atlas.
layout(set = 0, binding = 26) uniform highp sampler3D uIrrVol;

// The gain applied to the stored irradiance at shading time. The bake stores
// the raw sum of colour*intensity*attenuation*visibility from each emitter -
// the same terms applyLights adds for the DIRECT component - so this constant
// is the only place the indirect weight is set. It is a tuned constant, not a
// derived one: the bake has no notion of surface albedo or of the cosine the
// receiving surface will present, so the physically-normalised value would need
// an albedo and a normal term applied on top anyway. Keep it a single named
// constant so it is one edit, and one place a future session looks.
const float kIrrVolumeGain = 0.55;

// pc.b.x is the world size in metres (the same value heightAt and objDist use),
// and the bake uses the identical mapping: texel centre at world centre,
// clamped to the volume. Sharing the expression rather than hardcoding 102.4 is
// what keeps the CPU and GPU agreeing if WORLD ever changes.
//
// INCLUDE ORDER IS LOAD-BEARING. This uses `pc`, which is declared by the
// ENTRY POINT (splat.frag, svo_raymarch.comp) and not by any shared header, so
// this file must be included after that declaration. Verified: included after
// the push block it compiles clean; included before it, glslangValidator fails
// with "'pc' : undeclared identifier" on the line below, plus a bogus "vector
// swizzle selection out of range" on `b` that looks like a typo in this file
// rather than an ordering mistake. That is the whole reason for this note.
vec3 irrVolumeUVW(vec3 p)
{
    return clamp(p / pc.b.x + 0.5, vec3(0.0), vec3(1.0));
}

// Trilinear RGB irradiance arriving at p, already multiplied by the gain.
// Safe when the volume was never uploaded (VF_TEXTURES=0 or a pass that does
// not own the binding): a zeroed sampler reads 0, which makes the caller's
// fallback to the old analytic term a no-op rather than a special case.
vec3 irradianceVolume(vec3 p)
{
    return texture(uIrrVol, irrVolumeUVW(p)).rgb * kIrrVolumeGain;
}

// Sky-presence fraction in [0,1]: how much of the open sky this point can
// see. Separate from the RGB on purpose - the sky's own radiance is added
// analytically by skyIrradiance() at shading time, so this is only the
// occlusion factor between "indoors" and "outdoors". Using it as a blend
// weight is what stops a sealed cave with one lamp in it from also receiving
// full daylight ambient, which is the failure the bit-8 enclosure test
// exists to prevent.
float irradianceVolumeSky(vec3 p)
{
    return clamp(texture(uIrrVol, irrVolumeUVW(p)).a, 0.0, 1.0);
}

#endif // VF_COMMON_IRRADIANCE_GLSL