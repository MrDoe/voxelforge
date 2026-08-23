#include "voxel/volume.hpp"
#include <thread>
#include <algorithm>

namespace vf::voxel {

void DenseVolume::generate()
{
    const size_t voxels = size_t(n) * n * n;
    sdf.assign(voxels, 0.0f);
    albedo.assign(voxels * 3, 0);
    matId.assign(voxels, 0);

    float ws = worldSize;
    glm::vec3 off = originOffset;

    auto evalAt = [&](int x, int y, int z) {
        glm::vec3 p((x + 0.5f) / n * ws - 0.5f * ws, (y + 0.5f) / n * ws - 0.5f * ws,
                    (z + 0.5f) / n * ws - 0.5f * ws);
        SceneSample s = scene(p + off);
        size_t i = (size_t(z) * n + y) * n + x;
        sdf[i] = s.d;
        matId[i] = s.mat;
        const glm::vec3& c = kPalette[s.mat];
        albedo[i * 3 + 0] = uint8_t(c.r * 255.0f);
        albedo[i * 3 + 1] = uint8_t(c.g * 255.0f);
        albedo[i * 3 + 2] = uint8_t(c.b * 255.0f);
    };

    unsigned hc = std::max(1u, std::thread::hardware_concurrency());
    int slabs = std::min(n, int(hc));
    std::vector<std::thread> threads;
    int chunk = (n + slabs - 1) / slabs;
    for (int s0 = 0; s0 < n; s0 += chunk) {
        int e = std::min(n, s0 + chunk);
        threads.emplace_back([&, s0, e] {
            for (int z = s0; z < e; ++z)
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                        evalAt(x, y, z);
        });
    }
    for (auto& t : threads)
        t.join();
}

float DenseVolume::sampleTrilinear(glm::vec3 localP) const
{
    glm::vec3 g((localP / worldSize + 0.5f) * float(n) - 0.5f);
    g = glm::clamp(g, glm::vec3(0.0f), glm::vec3(n - 1.001f));
    glm::ivec3 i0 = glm::ivec3(g);
    glm::vec3 f = g - glm::vec3(i0);

    auto at = [&](glm::ivec3 c) -> float {
        c = glm::clamp(c, glm::ivec3(0), glm::ivec3(n - 1));
        return sdf[(size_t(c.z) * n + c.y) * n + c.x];
    };

    float c00 = glm::mix(at({ i0.x, i0.y, i0.z }), at({ i0.x + 1, i0.y, i0.z }), f.x);
    float c10 = glm::mix(at({ i0.x, i0.y + 1, i0.z }), at({ i0.x + 1, i0.y + 1, i0.z }), f.x);
    float c01 = glm::mix(at({ i0.x, i0.y, i0.z + 1 }), at({ i0.x + 1, i0.y, i0.z + 1 }), f.x);
    float c11 = glm::mix(at({ i0.x, i0.y + 1, i0.z + 1 }), at({ i0.x + 1, i0.y + 1, i0.z + 1 }), f.x);
    return glm::mix(glm::mix(c00, c10, f.y), glm::mix(c01, c11, f.y), f.z);
}

} // namespace vf::voxel
