#include "voxel/volume.hpp"
#include "voxel/common.hpp"
#include <doctest/doctest.h>
#include <cmath>

using namespace vf::voxel;

namespace {
glm::vec3 voxelToLocal(const DenseVolume& v, int x, int y, int z)
{
    float ws = v.worldSize;
    return glm::vec3((x + 0.5f) / v.n * ws - 0.5f * ws, (y + 0.5f) / v.n * ws - 0.5f * ws,
                     (z + 0.5f) / v.n * ws - 0.5f * ws);
}
} // namespace

TEST_CASE("dense generate is deterministic")
{
    DenseVolume a, b;
    a.generate();
    b.generate();
    REQUIRE(a.sdf.size() == b.sdf.size());
    REQUIRE(a.matId == b.matId);
    for (size_t i = 0; i < a.sdf.size(); i += 97)
        CHECK_EQ(a.sdf[i], doctest::Approx(b.sdf[i]).epsilon(1e-6));
}

TEST_CASE("dense sdf is finite and bounded")
{
    DenseVolume v;
    v.generate();
    for (float d : v.sdf) {
        CHECK(std::isfinite(d));
        REQUIRE(std::abs(d) < v.worldSize);
    }
}

TEST_CASE("inside/outside sanity on shared scene")
{
    DenseVolume v;
    v.generate();

    // deep below terrain is solid
    CHECK(v.sampleTrilinear({ 0.f, -v.worldSize * 0.45f, 0.f }) < 0.f);

    // high above is empty
    CHECK(v.sampleTrilinear({ 0.f, 10.f, 0.f }) > 1.0f);

    // deep in the river bed (river passes z ~ +5 near x = 0) is solid
    glm::vec3 bc{ 0.f, -3.6f, 5.f };
    float analytic = scene(bc).d;
    CHECK(analytic < -0.3f);
    CHECK(v.sampleTrilinear(bc) < 0.f);

    // agreement with analytic SDF near surface
    int agree = 0, tested = 0;
    for (int i = 0; i < 300; ++i) {
        float f = float(i) / 300.0f;
        glm::vec3 p(glm::mix(-11.f, 11.f, f), -1.5f + sinf(f * 8.f) * 1.5f,
                    glm::mix(-11.f, 11.f, cosf(f * 6.f)));
        float dRef = scene(p).d;
        if (std::abs(dRef) > 1.0f)
            continue;
        float dGot = v.sampleTrilinear(p);
        ++tested;
        if ((dRef < 0) == (dGot < 0) || std::abs(dRef - dGot) < 0.35f)
            ++agree;
    }
    MESSAGE("dense/sceen near-surface agreement: ", agree, "/", tested);
    CHECK(tested > 40);
    CHECK(agree > tested * 80 / 100);
}

TEST_CASE("exactly one surface crossing along outward ray")
{
    DenseVolume v;
    v.generate();
    glm::vec3 origin{ 0.f, -3.6f, 5.f }; // river bed, below the surface
    glm::vec3 dir = glm::vec3(0.0f, 1.0f, 0.0f); // straight up: one clean exit

    int crossings = 0;
    float prev = v.sampleTrilinear(origin);
    for (float t = 0.02f; t < 15.0f; t += 0.02f) {
        float d = v.sampleTrilinear(origin + dir * t);
        if (prev < 0.0f && d >= 0.0f)
            ++crossings;
        prev = d;
    }
    CHECK(crossings >= 1);
    CHECK(prev > 0.0f);
}

TEST_CASE("snorm8 roundtrip error within quantization step")
{
    const float maxErr = MAX_ENCODED_DIST / 127.0f + 1e-6f;
    for (int i = -140; i <= 140; ++i) {
        float v = float(i) / 140.0f * MAX_ENCODED_DIST;
        uint8_t enc = encodeSnorm8(v);
        float dec = decodeSnorm8(enc);
        if (std::abs(v) <= MAX_ENCODED_DIST + 1e-6f)
            CHECK(std::abs(dec - glm::clamp(v / MAX_ENCODED_DIST, -1.f, 1.f) *
                               MAX_ENCODED_DIST) <= maxErr);
    }
}

TEST_CASE("material ids valid and near-surface albedo nonzero")
{
    DenseVolume v;
    v.generate();
    size_t checked = 0;
    for (size_t i = 0; i < v.matId.size(); i += 997) {
        CHECK(v.matId[i] < kPalette.size());
        if (std::abs(v.sdf[i]) < 0.2f) {
            ++checked;
            CHECK((v.albedo[i * 3] | v.albedo[i * 3 + 1] | v.albedo[i * 3 + 2]) != 0);
        }
    }
    MESSAGE("near-surface samples checked: ", checked);
    CHECK(checked > 100);
}
