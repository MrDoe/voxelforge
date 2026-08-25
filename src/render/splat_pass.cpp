#include "render/splat_pass.hpp"
#include "rhi/resources.hpp"
#include <core/log.hpp>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>

#include "stb_image.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace vf {

namespace {

std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::critical("Cannot open shader '{}'", path);
        return {};
    }
    size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> d(n);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(n));
    return d;
}

VkShaderModule createModule(VkDevice dev, const std::vector<uint8_t>& spirv)
{
    VkShaderModuleCreateInfo ci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

// Hammersley
float radicalInverseVdC(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}
glm::vec2 hammersley(uint32_t i, uint32_t N) {
    return glm::vec2(float(i)/float(N), radicalInverseVdC(i));
}

glm::vec3 importanceSampleGGX(glm::vec2 Xi, glm::vec3 N, float roughness) {
    float a = roughness*roughness;
    float phi = 2.0f * glm::pi<float>() * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a*a - 1.0f)*Xi.y));
    float sinTheta = sqrt(1.0f - cosTheta*cosTheta);
    glm::vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
    glm::vec3 up = fabs(N.z) < 0.999f ? glm::vec3(0,0,1) : glm::vec3(1,0,0);
    glm::vec3 tangentX = glm::normalize(glm::cross(up, N));
    glm::vec3 tangentY = glm::cross(N, tangentX);
    return tangentX * H.x + tangentY * H.y + N * H.z;
}

} // namespace

// Cubemap helpers
glm::vec3 SplatPass::cubemapDir(int face, float u, float v) {
    // u,v in [-1,1]
    switch(face){
        case 0: return glm::normalize(glm::vec3(1.0f, -v, -u)); // +X
        case 1: return glm::normalize(glm::vec3(-1.0f, -v, u)); // -X
        case 2: return glm::normalize(glm::vec3(u, 1.0f, v)); // +Y
        case 3: return glm::normalize(glm::vec3(u, -1.0f, -v)); // -Y
        case 4: return glm::normalize(glm::vec3(u, -v, 1.0f)); // +Z
        case 5: return glm::normalize(glm::vec3(-u, -v, -1.0f)); // -Z
        default: return glm::vec3(0,1,0);
    }
}

glm::vec3 SplatPass::sampleCubemapDir(const glm::vec3& dir,
                                      const std::vector<std::vector<glm::vec4>>& faces,
                                      int baseSize) {
    float absX = fabs(dir.x), absY = fabs(dir.y), absZ = fabs(dir.z);
    int face = 0;
    float uc=0, vc=0, ma=0;
    if (absX >= absY && absX >= absZ) {
        ma = absX;
        if (dir.x > 0) { face=0; uc = -dir.z / ma; vc = -dir.y / ma; }
        else { face=1; uc = dir.z / ma; vc = -dir.y / ma; }
    } else if (absY >= absX && absY >= absZ) {
        ma = absY;
        if (dir.y > 0) { face=2; uc = dir.x / ma; vc = dir.z / ma; }
        else { face=3; uc = dir.x / ma; vc = -dir.z / ma; }
    } else {
        ma = absZ;
        if (dir.z > 0) { face=4; uc = dir.x / ma; vc = -dir.y / ma; }
        else { face=5; uc = -dir.x / ma; vc = -dir.y / ma; }
    }
    // uc,vc in [-1,1] -> texel coord
    float u = (uc * 0.5f + 0.5f) * (baseSize - 1);
    float v = (vc * 0.5f + 0.5f) * (baseSize - 1);
    int x0 = int(floor(u)), y0 = int(floor(v));
    int x1 = std::min(x0+1, baseSize-1), y1 = std::min(y0+1, baseSize-1);
    x0 = std::clamp(x0, 0, baseSize-1); y0 = std::clamp(y0, 0, baseSize-1);
    float fx = u - float(x0), fy = v - float(y0);
    const auto& f = faces[face];
    glm::vec4 c00 = f[y0*baseSize + x0];
    glm::vec4 c10 = f[y0*baseSize + x1];
    glm::vec4 c01 = f[y1*baseSize + x0];
    glm::vec4 c11 = f[y1*baseSize + x1];
    glm::vec4 c0 = glm::mix(c00, c10, fx);
    glm::vec4 c1 = glm::mix(c01, c11, fx);
    glm::vec4 c = glm::mix(c0, c1, fy);
    return glm::vec3(c);
}

bool SplatPass::loadOrGenerateEnvFaces(std::vector<std::vector<glm::vec4>>& outFaces, int size) {
    outFaces.assign(6, std::vector<glm::vec4>(size*size));
    std::string hdrPath = std::string(VOXELFORGE_ASSET_DIR) + "/env.hdr";
    int w=0,h=0,comp=0;
    float* hdr = stbi_loadf(hdrPath.c_str(), &w, &h, &comp, 3);
    if (hdr && w>0 && h>0) {
        spdlog::info("env: loaded HDR {} ({}x{} comp {})", hdrPath, w,h,comp);
        // hdr is equirectangular latlong
        for(int face=0; face<6; ++face){
            for(int y=0; y<size; ++y){
                for(int x=0; x<size; ++x){
                    float u = (2.0f*(x+0.5f)/size -1.0f);
                    float v = (2.0f*(y+0.5f)/size -1.0f);
                    glm::vec3 dir = cubemapDir(face,u,v);
                    dir = glm::normalize(dir);
                    float phi = atan2(dir.z, dir.x);
                    if(phi<0) phi += 2.0f*glm::pi<float>();
                    float theta = acos(std::clamp(dir.y, -1.0f, 1.0f));
                    float uf = phi / (2.0f*glm::pi<float>());
                    float vf = theta / glm::pi<float>();
                    float sx = uf * (w-1);
                    float sy = vf * (h-1);
                    int x0 = int(floor(sx)), y0 = int(floor(sy));
                    int x1 = std::min(x0+1, w-1), y1 = std::min(y0+1, h-1);
                    x0 = std::clamp(x0,0,w-1); y0=std::clamp(y0,0,h-1);
                    float fx = sx - x0, fy = sy - y0;
                    auto fetch = [&](int ix,int iy)->glm::vec3{
                        int idx = (iy*w+ix)*3;
                        return glm::vec3(hdr[idx], hdr[idx+1], hdr[idx+2]);
                    };
                    glm::vec3 c00=fetch(x0,y0), c10=fetch(x1,y0), c01=fetch(x0,y1), c11=fetch(x1,y1);
                    glm::vec3 c0 = glm::mix(c00,c10,fx);
                    glm::vec3 c1 = glm::mix(c01,c11,fx);
                    glm::vec3 c = glm::mix(c0,c1,fy);
                    outFaces[face][y*size+x] = glm::vec4(c,1.0f);
                }
            }
        }
        stbi_image_free(hdr);
        return true;
    } else {
        if(hdr) stbi_image_free(hdr);
        spdlog::warn("env: {} not found, generating procedural sky cubemap", hdrPath);
        // procedural sky using same Hosek-like model as raymarch
        glm::vec3 sunDir = glm::normalize(glm::vec3(0.449f, 0.8338f, 0.3207f)); // default 34/238
        const glm::vec3 kSunCol = glm::vec3(1.00f,0.95f,0.84f)*1.35f;
        const glm::vec3 kZenith = glm::vec3(0.20f,0.36f,0.62f);
        const glm::vec3 kHorizon = glm::vec3(0.72f,0.80f,0.90f);
        const float T = 2.2f;
        float Ay = 0.1787f*T -1.4630f;
        float By = -0.3554f*T +0.4275f;
        float Cy = -0.0227f*T +5.3251f;
        float Dy = 0.1206f*T -2.5771f;
        float Ey = -0.0670f*T +0.3703f;
        auto skyColor = [&](glm::vec3 d)->glm::vec3{
            float cosTheta = std::clamp(d.y, 0.0f, 1.0f);
            float cosGamma = std::max(glm::dot(d, sunDir), 0.0f);
            float gamma = acos(std::clamp(cosGamma,0.0f,1.0f));
            float cosThetaSafe = std::max(cosTheta,0.07f);
            float Ftheta = (1.0f + Ay*exp(By/cosThetaSafe))/(1.0f+Ay*exp(By));
            float Fgamma = 1.0f+Cy*exp(Dy*gamma)+Ey*cosGamma*cosGamma;
            float Y = Ftheta*Fgamma;
            glm::vec3 base = glm::mix(kHorizon*1.05f, kZenith*0.95f, powf(std::max(cosTheta,0.0f),0.55f));
            glm::vec3 col = base*(0.82f+0.30f*std::clamp(Y*0.08f,0.0f,1.5f));
            col += kSunCol*0.55f*powf(cosGamma,1150.0f)*std::clamp(Y*0.15f,0.0f,2.0f);
            col += glm::vec3(1.0f,0.85f,0.6f)*0.48f*powf(cosGamma,6.0f)*std::clamp(Y*0.06f,0.2f,1.2f);
            // ground
            if(d.y<0) {
                float t = std::clamp(-d.y*3.0f,0.0f,1.0f);
                col = glm::mix(col, glm::vec3(0.06f,0.10f,0.08f), t*0.6f);
            }
            return col;
        };
        for(int face=0; face<6; ++face){
            for(int y=0;y<size;++y){
                for(int x=0;x<size;++x){
                    float u = 2.0f*(x+0.5f)/size -1.0f;
                    float v = 2.0f*(y+0.5f)/size -1.0f;
                    glm::vec3 dir = cubemapDir(face,u,v);
                    glm::vec3 col = skyColor(glm::normalize(dir));
                    // boost HDR range a bit
                    col *= 1.2f;
                    // ensure sun is HDR bright
                    outFaces[face][y*size+x]=glm::vec4(col,1.0f);
                }
            }
        }
        return true;
    }
}

bool SplatPass::createEnvCubemap() {
    const int baseSize = 64;
    const int mipLevels = 7; // 64..1
    std::vector<std::vector<glm::vec4>> baseFaces;
    if (!loadOrGenerateEnvFaces(baseFaces, baseSize)) return false;

    // Generate prefiltered mips via GGX importance sampling
    // storage: mips[ level ][ face ][ texels ] as vec4
    struct MipFace { std::vector<glm::vec4> data; int size; };
    std::vector<std::vector<MipFace>> mips(mipLevels);
    for(int l=0;l<mipLevels;++l){
        int sz = std::max(1, baseSize >> l);
        mips[l].resize(6);
        for(int f=0;f<6;++f){ mips[l][f].size=sz; mips[l][f].data.assign(sz*sz, glm::vec4(0)); }
    }
    // level 0 copy
    for(int f=0;f<6;++f) mips[0][f].data = baseFaces[f];

    const uint32_t SAMPLE_COUNT = 256; // quality vs startup time
    for(int l=1; l<mipLevels; ++l){
        int sz = std::max(1, baseSize >> l);
        float roughness = float(l)/float(mipLevels-1);
        // float alpha = roughness*roughness;
        spdlog::info("env prefilter mip {} size {} roughness {:.2f}", l, sz, roughness);
        for(int face=0; face<6; ++face){
            for(int y=0;y<sz;++y){
                for(int x=0;x<sz;++x){
                    float u = 2.0f*(x+0.5f)/sz -1.0f;
                    float v = 2.0f*(y+0.5f)/sz -1.0f;
                    glm::vec3 R = cubemapDir(face,u,v);
                    R = glm::normalize(R);
                    glm::vec3 N = R;
                    glm::vec3 V = R;
                    glm::vec3 prefiltered(0.0f);
                    float totalWeight=0.0f;
                    for(uint32_t i=0;i<SAMPLE_COUNT;++i){
                        glm::vec2 Xi = hammersley(i, SAMPLE_COUNT);
                        glm::vec3 H = importanceSampleGGX(Xi, N, roughness);
                        glm::vec3 L = glm::normalize(2.0f*glm::dot(V,H)*H - V);
                        float NdotL = std::max(glm::dot(N,L), 0.0f);
                        if(NdotL>0.0f){
                            glm::vec3 sample = sampleCubemapDir(L, baseFaces, baseSize);
                            // attenuate by NdotL as in split-sum prefilter
                            prefiltered += sample * NdotL;
                            totalWeight += NdotL;
                        }
                    }
                    if(totalWeight>0) prefiltered /= totalWeight;
                    // fallback if no sample (should not happen)
                    if(totalWeight==0) prefiltered = glm::vec3(baseFaces[face][0]);
                    // Apply simple roughness-based energy compensation (optional)
                    mips[l][face].data[y*sz + x] = glm::vec4(prefiltered,1.0f);
                }
            }
        }
    }

    // Create Vulkan cube image R16G16B16A16_SFLOAT, mipLevels
    VkDevice dev = m_ctx->device();
    VkImageCreateInfo ii { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = { (uint32_t)baseSize, (uint32_t)baseSize, 1 };
    ii.mipLevels = mipLevels;
    ii.arrayLayers = 6;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &m_envCube, &m_envAlloc, nullptr)!= VK_SUCCESS){
        spdlog::critical("env cube image creation failed");
        return false;
    }
    // Actually re-create as 32F for simplicity - destroy and recreate
    vmaDestroyImage(m_ctx->allocator(), m_envCube, m_envAlloc);
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &m_envCube, &m_envAlloc, nullptr)!= VK_SUCCESS){
        spdlog::critical("env cube 32F creation failed");
        return false;
    }
    m_envMips = mipLevels;
    // prepare staging per mip-face as 32F
    std::vector<Buffer> stagingBuffers;
    stagingBuffers.reserve(6*mipLevels);
    for(int l=0;l<mipLevels;++l){
        for(int f=0;f<6;++f){
            auto& mf = mips[l][f].data;
            size_t bytes = mf.size()*sizeof(glm::vec4);
            Buffer st = makeBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
            if(!st.buf) return false;
            memcpy(st.mapped, mf.data(), bytes);
            stagingBuffers.push_back(st);
        }
    }
    bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd){
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = m_envCube;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount=1; dep.pImageMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&dep);
        int idx=0;
        for(int l=0;l<mipLevels;++l){
            int sz = std::max(1, baseSize >> l);
            for(int f=0;f<6;++f){
                VkBufferImageCopy region{};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, (uint32_t)f, 1 };
                region.imageOffset = {0,0,0};
                region.imageExtent = { (uint32_t)sz, (uint32_t)sz, 1 };
                vkCmdCopyBufferToImage(cmd, stagingBuffers[idx].buf, m_envCube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                ++idx;
            }
        }
        VkImageMemoryBarrier2 b2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b2.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.image = m_envCube;
        b2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
        VkDependencyInfo dep2{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep2.imageMemoryBarrierCount=1; dep2.pImageMemoryBarriers=&b2;
        vkCmdPipelineBarrier2(cmd,&dep2);
    });
    for(auto &s: stagingBuffers) destroyBuffer(*m_ctx, s);
    if(!ok) return false;
    // create view + sampler
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_envCube;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6 };
    if(vkCreateImageView(dev,&vi,nullptr,&m_envView)!=VK_SUCCESS) return false;
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = VK_FALSE;
    si.maxAnisotropy = 1.0f;
    si.minLod = 0.0f;
    si.maxLod = float(mipLevels);
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if(vkCreateSampler(dev,&si,nullptr,&m_envSampler)!=VK_SUCCESS) return false;
    spdlog::info("env cubemap created {}x{}x6 mips {}", baseSize, baseSize, mipLevels);
    return true;
}

void SplatPass::destroyEnvCubemap() {
    if(!m_ctx) return;
    VkDevice dev = m_ctx->device();
    if(m_envSampler) vkDestroySampler(dev, m_envSampler, nullptr);
    if(m_envView) vkDestroyImageView(dev, m_envView, nullptr);
    if(m_envCube) vmaDestroyImage(m_ctx->allocator(), m_envCube, m_envAlloc);
    m_envSampler=VK_NULL_HANDLE; m_envView=VK_NULL_HANDLE; m_envCube=VK_NULL_HANDLE; m_envAlloc=VK_NULL_HANDLE;
}

bool SplatPass::init(const Context& ctx, VkFormat colorFormat, const SplatVertexData& data)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();
    m_count = data.posRadius.size();
    if (m_count == 0) {
        spdlog::warn("splat: no splats generated");
        return false;
    }

    m_origPos = data.posRadius;
    m_origCol = data.colors;
    m_origAlbedoAO = data.albedoAO;
    m_origNormalMat = data.normalMat;
    m_origShadeParams = data.shadeParams;
    m_origShadow = data.shadow;
    m_lastSortPos = glm::vec3(1e9f);
    m_lastSortFwd = glm::vec3(0,0,1);
    m_lastSortFrame = 0;
    bool hasNew = (data.albedoAO.size() == m_count && data.normalMat.size() == m_count);
    bool hasShade = hasNew && data.shadeParams.size()==m_count;
    bool hasShadow = hasNew && data.shadow.size()==m_count;
    size_t stride = hasNew ? 5 : 2;
    std::vector<glm::vec4> packed(m_count * stride);
    for (size_t i = 0; i < m_count; ++i) {
        packed[stride * i + 0] = data.posRadius[i];
        if (hasNew) {
            packed[stride * i + 1] = data.albedoAO[i];
            packed[stride * i + 2] = data.normalMat[i];
            if(hasShade) packed[stride*i+3] = data.shadeParams[i];
            else {
                int mat = int(data.normalMat[i].w + 0.5f);
                mat = std::clamp(mat, 0, 8);
                const float kRefl[9]={35,40,55,130,95,115,70,60,30};
                const float kRough[9]={235,230,225,190,150,135,160,170,235};
                float refl = kRefl[mat]/255.0f;
                float rough = kRough[mat]/255.0f;
                packed[stride*i+3] = glm::vec4(refl, refl*0.9f, refl*0.8f, rough);
            }
            if(hasShadow) packed[stride*i+4] = glm::vec4(data.shadow[i],0,0,0);
            else packed[stride*i+4] = glm::vec4(0,0,0,0);
        } else {
            packed[stride * i + 1] = data.colors[i];
        }
    }

    Buffer staging =
        makeBuffer(ctx, packed.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    memcpy(staging.mapped, packed.data(), packed.size() * sizeof(glm::vec4));

    auto bufTmp = makeBuffer(ctx, packed.size() * sizeof(glm::vec4),
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, false);
    m_buf = bufTmp.buf;
    m_alloc = bufTmp.alloc;
    if (!m_buf) {
        spdlog::critical("splat ssbo alloc failed");
        return false;
    }
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, 0, packed.size() * sizeof(glm::vec4) };
        vkCmdCopyBuffer(cmd, staging.buf, m_buf, 1, &c);
    });
    destroyBuffer(ctx, staging);

    if(!createEnvCubemap()){
        spdlog::warn("splat: env cubemap creation failed - continuing without env");
    }

    // descriptor set layout: 0 storage buffer (vert), 1 cubemap sampler (frag)
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    uint32_t bindCount = m_envView ? 2 : 1;
    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = bindCount;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize ps[2]{};
    ps[0] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    ps[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = bindCount;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    VkDescriptorBufferInfo bi { m_buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w0 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr };
    if(m_envView){
        VkDescriptorImageInfo ii{ m_envSampler, m_envView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w1{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 1, 0, 1,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, nullptr, nullptr };
        VkWriteDescriptorSet ws[2]={w0,w1};
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    } else {
        vkUpdateDescriptorSets(dev, 1, &w0, 0, nullptr);
    }

    // shaders
    auto vs = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.vert.spv");
    auto fs = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/splat.frag.spv");
    if (vs.empty() || fs.empty())
        return false;
    VkShaderModule vsm = createModule(dev, vs);
    VkShaderModule fsm = createModule(dev, fs);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    }; // fetch in-shader

    VkPipelineInputAssemblyStateCreateInfo ia {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo vp { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    }; // no depth buffer: painter-style blending only

    VkPipelineColorBlendAttachmentState cba {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;               // premultiplied alpha
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prc { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prc.colorAttachmentCount = 1;
    prc.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo gpi { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.pNext = &prc;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dy;
    gpi.layout = m_layout;

    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, vsm, nullptr);
    vkDestroyShaderModule(dev, fsm, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("splat: graphics pipeline failed ({})", int(r));
        return false;
    }
    spdlog::info("SplatPass ready: {} surface splats (env {})", m_count, m_envView?"HDR":"none");
    return true;
}

void SplatPass::record(VkCommandBuffer cmd, VkExtent2D extent, const RaymarchPush& push) const
{
    VkClearValue clear {};
    clear.color = { { 0.86f, 0.90f, 0.95f, 1.0f } }; // gamma-encoded horizon backdrop

    VkRenderingAttachmentInfo att { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    att.imageView = m_outView;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue = clear;

    VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;

    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
    VkRect2D sc { { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, uint32_t(m_count), 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void SplatPass::updateSorting(const glm::vec3& camPos, const glm::vec3& camFwd)
{
    if (m_count == 0 || m_origPos.empty()) return;
    // skip sort if camera barely moved (saves ~80% for static views, critical for 5-10M)
    float posDelta = glm::length(camPos - m_lastSortPos);
    float fwdDelta = 1.0f - glm::dot(glm::normalize(camFwd), glm::normalize(m_lastSortFwd));
    bool camStatic = posDelta < 0.02f && fwdDelta < 0.0005f;
    if (camStatic && m_lastSortFrame != 0) return;
    // for very dense, sort every 2nd frame when moving slowly
    if (m_count > 5000000 && (m_lastSortFrame % 2 == 0) && posDelta < 0.1f && fwdDelta < 0.002f) return;
    m_lastSortPos = camPos;
    m_lastSortFwd = camFwd;
    m_lastSortFrame++;
    bool hasNew = (m_origAlbedoAO.size() == m_count && m_origNormalMat.size() == m_count);
    bool hasShadow = hasNew && m_origShadow.size()==m_count;
    size_t stride = hasNew ? 5 : 2;
    std::vector<uint32_t> idx(m_count);
    std::iota(idx.begin(), idx.end(), 0u);
    // std::sort is ~30% faster than stable_sort; tie-breaker keeps determinism for close depths
    std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b){
        float da = glm::dot(glm::vec3(m_origPos[a]) - camPos, camFwd);
        float db = glm::dot(glm::vec3(m_origPos[b]) - camPos, camFwd);
        if (std::abs(da - db) < 0.005f) return a < b;
        return da > db;
    });
    std::vector<glm::vec4> packed(m_count * stride);
    for (size_t i = 0; i < m_count; ++i) {
        uint32_t s = idx[i];
        packed[stride*i+0] = m_origPos[s];
        if (hasNew) {
            packed[stride*i+1] = m_origAlbedoAO[s];
            packed[stride*i+2] = m_origNormalMat[s];
            if(m_origShadeParams.size()==m_count) packed[stride*i+3] = m_origShadeParams[s];
            else {
                int mat = int(m_origNormalMat[s].w + 0.5f);
                mat = std::clamp(mat, 0, 8);
                const float kRefl[9]={35,40,55,130,95,115,70,60,30};
                const float kRough[9]={235,230,225,190,150,135,160,170,235};
                float refl = kRefl[mat]/255.0f;
                float rough = kRough[mat]/255.0f;
                packed[stride*i+3] = glm::vec4(refl, refl*0.9f, refl*0.8f, rough);
            }
            if(hasShadow) packed[stride*i+4] = glm::vec4(m_origShadow[s],0,0,0);
            else packed[stride*i+4] = glm::vec4(0,0,0,0);
        } else {
            packed[stride*i+1] = m_origCol[s];
        }
    }
    Buffer staging = makeBuffer(*m_ctx, packed.size()*sizeof(glm::vec4),
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    memcpy(staging.mapped, packed.data(), packed.size()*sizeof(glm::vec4));
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd){
        VkBufferCopy c{0,0, packed.size()*sizeof(glm::vec4)};
        vkCmdCopyBuffer(cmd, staging.buf, m_buf, 1, &c);
    });
    destroyBuffer(*m_ctx, staging);
}

void SplatPass::destroy()
{
    destroyEnvCubemap();
    VkDevice dev = m_ctx ? m_ctx->device() : VK_NULL_HANDLE;
    if (!dev)
        return;
    if (m_buf)
        vmaDestroyBuffer(m_ctx->allocator(), m_buf, m_alloc);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
    m_buf = VK_NULL_HANDLE;
    m_alloc = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
}

} // namespace vf
