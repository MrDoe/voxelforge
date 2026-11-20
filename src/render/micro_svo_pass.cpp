// micro_svo_pass.cpp - upload and draw the SEPARATE micro-voxel SVO worlds.
#include "render/micro_svo_pass.hpp"
#include "render/svo_pass.hpp"
#include "voxel/world.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>

namespace vf {

namespace {

// The mesh pass's own depth attachment. D32 is a mandatory depth-attachment
// format, and 24 bits of a D24_S8 would clamp a 200 m linear range to 1 cm
// steps - enough to z-fight a 5 mm lattice against itself.
constexpr VkFormat kMicroMeshDepthFormat = VK_FORMAT_D32_SFLOAT;

// Same per-file helper the other passes carry (each render translation unit has
// its own; it is four lines and not worth a header).
std::vector<uint8_t> loadSpirv(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        spdlog::critical("Cannot open shader '{}'", path);
        return {};
    }
    const size_t n = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> d(n);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(n));
    return d;
}

// Mirrors the std140 MicroWorld struct in micro_overlay.comp: geo, base, lo, hi
// - four 16-byte slots per world. kMicroMaxWorlds is the shader's table size.
constexpr int kMicroMaxWorlds = 16;
// One bone: the segment it occupies, and its CHAINED transform - a rigid
// one (R*p + t), because the composition of two pivot-rotations is not a
// pivot-rotation, and a child that turns about a rest joint while the parent
// moves tears at the seam.
struct alignas(16) MicroBoneRow {
    float a[4];       // the segment's start
    float b[4];       // the segment's end
    float xform[4];   // the chained transform's translation (xyz)
    float r0[4];      // the chained transform's rotation (row-major)
    float r1[4];
    float r2[4];
};

struct alignas(16) MicroWorldRow {
    // geo  = voxel, gridN, chunkM, dimY
    // base = gridBase, chunkInfoBase, dimX, dimZ   <- authored cell dims
    // lo/hi = world AABB min/max. lo.w carries the world's OFFSET into the
    // dense field buffer (in cells); hi.w is unused.
    // The cell dims matter: a chunk spans 16 cells, so the outermost chunks of
    // a small object reach past the authored grid. Those cells exist in the
    // brick but have no record behind them, and the shader must clamp its cell
    // index to the grid or it samples a filler cell - which reads as grass
    // material and turns a pale ceramic vase green.
    // preview = the live preview offset (xyz) + an active flag (w): a staged
    // Move delta for the selected micro object. The field stays where it was
    // baked and the RENDERED position is offset, so a drag is instant and
    // costs no re-bake.
    // rot0/rot1/rot2 = the live ROTATION preview's matrix (row-major) and
    // pivot = the pivot (xyz, world m) + an active flag (w). Same idea: the
    // field, the material lookup and the octree stay in the BAKED frame and
    // the RENDERED position is rotated out of it. A micro object has no world
    // owner id, so the splat pass's rotate preview cannot show it - this is
    // its only live rotation.
    // anim = the AUTONOMOUS BEHAVIOUR's offset (xyz) + an active flag (w);
    // arot0..2 = its rotation matrix (row-major) and apivot = its pivot
    // (xyz, world m). Same contract as the rotation preview: the field stays
    // in the baked frame and the transform is applied when it is sampled, so
    // a walk costs no re-bake.
    float geo[4];
    uint32_t base[4];
    float lo[4];
    float hi[4];
    float preview[4];
    float rot0[4];
    float rot1[4];
    float rot2[4];
    float pivot[4];
    float anim[4];
    float arot0[4];
    float arot1[4];
    float arot2[4];
    float apivot[4];
    // bones = the SKELETON: x = the bone count. The bones follow: their
    // SEGMENT and JOINT never change, and the gait rewrites only their
    // rotations.
    float bones[4];
    // 12 slots, not 8: a quadruped rig is already 7 bones (spine, 4 legs,
    // tail, head) and the neck is a CHAIN of three, so 8 would silently drop
    // the head - and every bone index above the cut, including the parents
    // the shader composes, would name the wrong bone.
    MicroBoneRow bone[12];
};
static_assert(sizeof(MicroWorldRow) == 1392, "std140 slot size");

} // namespace

bool MicroSvoPass::uploadSsbo(Ssbo& s, const void* data, size_t bytes)
{
    destroySsbo(s);
    if (!data || bytes == 0)
        return true;
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(m_ctx->allocator(), &bi, &ai, &s.buf, &s.alloc, nullptr) !=
        VK_SUCCESS)
        return false;
    s.bytes = bytes;
    Buffer staging = makeBuffer(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!staging.buf || !staging.mapped)
        return false;
    memcpy(staging.mapped, data, bytes);
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy c { 0, 0, bytes };
        vkCmdCopyBuffer(cmd, staging.buf, s.buf, 1, &c);
    });
    destroyBuffer(*m_ctx, staging);
    return ok;
}

void MicroSvoPass::destroySsbo(Ssbo& s)
{
    if (s.buf)
        vmaDestroyBuffer(m_ctx->allocator(), s.buf, s.alloc);
    s.buf = VK_NULL_HANDLE;
    s.alloc = VK_NULL_HANDLE;
    s.bytes = 0;
}

void MicroSvoPass::bindMicroBuffers()
{
    VkDescriptorBufferInfo infos[7] = {
        { m_grid.buf, 0, VK_WHOLE_SIZE },    { m_childBase.buf, 0, VK_WHOLE_SIZE },
        { m_payload.buf, 0, VK_WHOLE_SIZE }, { m_handles.buf, 0, VK_WHOLE_SIZE },
        { m_bricks.buf, 0, VK_WHOLE_SIZE },  { m_chunkInfo.buf, 0, VK_WHOLE_SIZE },
        { m_field.buf, 0, VK_WHOLE_SIZE },
    };
    const uint32_t bindings[7] = { 1, 2, 3, 4, 5, 6, 10 };
    VkWriteDescriptorSet w[7] = {};
    for (int i = 0; i < 7; ++i)
        w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, bindings[i], 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 7, w, 0, nullptr);
}

bool MicroSvoPass::setMicroWorlds(const std::vector<voxel::MicroWorld>& worlds)
{
    m_worldCount = 0;
    // The world list is being rebuilt, so every mesh flag dies with it: the
    // app uploads the meshes again after this call, and a world that no longer
    // has one must go back to the tracer rather than draw a stale buffer.
    m_meshWorldCount = 0;
    m_meshVerts.clear();
    if (worlds.empty()) {
        destroySsbo(m_grid);
        destroySsbo(m_childBase);
        destroySsbo(m_payload);
        destroySsbo(m_handles);
        destroySsbo(m_bricks);
        destroySsbo(m_chunkInfo);
        destroySsbo(m_field);
        if (m_worldTable.mapped)
            memset(m_worldTable.mapped, 0, sizeof(int32_t) * 4);
        return true;
    }
    if (int(worlds.size()) > kMicroMaxWorlds) {
        spdlog::error("micro: {} worlds exceeds the {}-world limit; the extra are "
                      "dropped",
                      worlds.size(), kMicroMaxWorlds);
    }
    const size_t n = std::min(worlds.size(), size_t(kMicroMaxWorlds));

    // Concatenate. Each world's chunk bases are REBASED onto the accumulated
    // arrays here, so the shader reaches every world's nodes/bricks with one
    // flat index and only needs the grid + chunkInfo bases from the table.
    std::vector<int32_t> grid;
    std::vector<voxel::GpuChunkInfo> info;
    std::vector<uint32_t> childBase, payload, handles, bricks;
    std::vector<uint8_t> field;
    std::vector<MicroWorldRow> rows;
    rows.reserve(n);
    for (size_t w = 0; w < n; ++w) {
        const voxel::MicroWorld& s = worlds[w];
        if (s.chunkGrid.empty())
            continue;
        MicroWorldRow row {};
        row.geo[0] = s.voxel;
        row.geo[1] = float(s.gridN);
        row.geo[2] = s.chunkM;
        row.geo[3] = float(s.fieldDim.y);
        row.base[2] = uint32_t(s.fieldDim.x);
        row.base[3] = uint32_t(s.fieldDim.z);
        row.base[0] = uint32_t(grid.size());
        row.base[1] = uint32_t(info.size());
        // The dense field is one contiguous array; each world's slice starts
        // where the previous one ended. The offset is padded to a multiple of
        // four because the shader reads the field as bytes out of a uint
        // array (std430 packs uints four to a block).
        while (field.size() % 4)
            field.push_back(0);
        row.lo[3] = float(field.size());
        field.insert(field.end(), s.field.begin(), s.field.end());
        for (int i = 0; i < 3; ++i) {
            row.lo[i] = s.worldMin[i];
            row.hi[i] = s.worldMax[i];
        }
        // Every world starts TRACED. setMesh() flips this to 1 for the ones
        // whose geometry is a posed mesh; a reload re-runs this loop, so a
        // layer that lost its mesh goes back to the tracer.
        row.hi[3] = 0.0f;
        row.preview[3] = 0.0f; // no live preview until one is set
        // The bones: their segments and joints (static) and the identity
        // rotation, which the gait then drives.
        const size_t kMaxBones = sizeof(row.bone) / sizeof(row.bone[0]);
        row.bones[0] = float(std::min(s.rig.bones.size(), kMaxBones));
        for (size_t bi = 0; bi < s.rig.bones.size() && bi < kMaxBones; ++bi) {
            const auto& b = s.rig.bones[bi];
            MicroBoneRow& br = row.bone[bi];
            br.a[0] = b.a.x; br.a[1] = b.a.y; br.a[2] = b.a.z;
            br.a[3] = b.radius; // per-bone capsule radius (for microBoneAt)
            br.b[0] = b.b.x; br.b[1] = b.b.y; br.b[2] = b.b.z;
            br.b[3] = 0.f;
            // The transform starts as the IDENTITY (no rotation, no
            // translation). It must NOT start as "the joint": the slot is
            // the transform's TRANSLATION, and a joint is a world position -
            // putting one there translated every object by metres (the vase
            // and the urn vanished from the scene). The behaviour pushes the
            // real transform every frame, and an object with no behaviour
            // keeps the identity.
            br.xform[0] = 0.f; br.xform[1] = 0.f; br.xform[2] = 0.f;
            br.r0[0] = 1.f;
            br.r1[1] = 1.f;
            br.r2[2] = 1.f;
        }
        // The rotation preview starts as the identity, inactive: pivot.w = 0
        // is what tells the shader there is nothing to rotate.
        row.rot0[0] = row.rot1[1] = row.rot2[2] = 1.0f;
        row.pivot[3] = 0.0f;
        // The behaviour's transform starts as the identity, inactive.
        row.arot0[0] = row.arot1[1] = row.arot2[2] = 1.0f;
        row.anim[3] = 0.0f;

        const uint32_t nodeOff = uint32_t(payload.size());
        const uint32_t handleOff = uint32_t(handles.size());
        const uint32_t brickOff = uint32_t(bricks.size() / voxel::BRICK_WORDS);
        for (voxel::GpuChunkInfo g : s.chunkInfo) {
            g.nodeBase += nodeOff;
            g.childBase += handleOff;
            g.brickBase += brickOff;
            info.push_back(g);
        }
        grid.insert(grid.end(), s.chunkGrid.begin(), s.chunkGrid.end());
        childBase.insert(childBase.end(), s.childBase.begin(), s.childBase.end());
        payload.insert(payload.end(), s.payload.begin(), s.payload.end());
        handles.insert(handles.end(), s.handles.begin(), s.handles.end());
        bricks.insert(bricks.end(), s.bricks.begin(), s.bricks.end());
        rows.push_back(row);
    }
    m_worldCount = rows.size();

    if (!uploadSsbo(m_grid, grid.data(), grid.size() * 4) ||
        !uploadSsbo(m_childBase, childBase.data(), childBase.size() * 4) ||
        !uploadSsbo(m_payload, payload.data(), payload.size() * 4) ||
        !uploadSsbo(m_handles, handles.data(), handles.size() * 4) ||
        !uploadSsbo(m_bricks, bricks.data(), bricks.size() * 4) ||
        !uploadSsbo(m_chunkInfo, info.data(), info.size() * 16) ||
        !uploadSsbo(m_field, field.data(), field.size())) {
        spdlog::error("micro: failed to upload one of the world buffers");
        m_worldCount = 0;
        return false;
    }
    bindMicroBuffers();

    // world table: rows then the count word
    if (m_worldTable.mapped) {
        std::memset(m_worldTable.mapped, 0, sizeof(MicroWorldRow) * kMicroMaxWorlds);
        memcpy(m_worldTable.mapped, rows.data(), sizeof(MicroWorldRow) * rows.size());
        int32_t* count = reinterpret_cast<int32_t*>(
            static_cast<uint8_t*>(m_worldTable.mapped) +
            sizeof(MicroWorldRow) * kMicroMaxWorlds);
        *count = int32_t(rows.size());
    }

    size_t totalBricks = 0;
    for (const auto& s : worlds)
        totalBricks += s.brickCount();
    spdlog::info("micro: {} world(s), {} bricks, {} grid chunks, {:.1f} KB gpu",
                 rows.size(), totalBricks, grid.size(),
                 double(gpuBytes()) / 1024.0);
    return true;
}

void MicroSvoPass::setWorldPreview(size_t worldIndex, glm::vec3 offset, bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    row.preview[0] = offset.x;
    row.preview[1] = offset.y;
    row.preview[2] = offset.z;
    row.preview[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setWorldRotation(size_t worldIndex, glm::mat3 R, glm::vec3 pivot,
                                    bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    // Row-major, like the splat pass's rotate preview (uRotRow0..2).
    row.rot0[0] = R[0][0]; row.rot0[1] = R[0][1]; row.rot0[2] = R[0][2];
    row.rot1[0] = R[1][0]; row.rot1[1] = R[1][1]; row.rot1[2] = R[1][2];
    row.rot2[0] = R[2][0]; row.rot2[1] = R[2][1]; row.rot2[2] = R[2][2];
    row.pivot[0] = pivot.x;
    row.pivot[1] = pivot.y;
    row.pivot[2] = pivot.z;
    row.pivot[3] = active ? 1.0f : 0.0f;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setBoneRotations(size_t worldIndex,
                                    const std::vector<glm::mat3>& rot,
                                    const std::vector<glm::vec3>& xform){
    // The gait rewrites the bones' rotations every frame. Only the matrix
    // part of each bone changes, so the world table is edited in place (the
    // segments and the joints stay as they were uploaded).
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    const int count = int(row.bones[0]);
    if (count <= 0 || int(rot.size()) < count)
        return;
    for (int i = 0; i < count; ++i) {
        const glm::mat3& R = rot[size_t(i)];
        MicroBoneRow& br = row.bone[i];
        br.r0[0] = R[0][0]; br.r0[1] = R[0][1]; br.r0[2] = R[0][2];
        br.r1[0] = R[1][0]; br.r1[1] = R[1][1]; br.r1[2] = R[1][2];
        br.r2[0] = R[2][0]; br.r2[1] = R[2][1]; br.r2[2] = R[2][2];
        if (size_t(i) < xform.size()) {
            br.xform[0] = xform[size_t(i)].x;
            br.xform[1] = xform[size_t(i)].y;
            br.xform[2] = xform[size_t(i)].z;
        }
    }
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setBoneSegments(size_t worldIndex,
                                   const std::vector<glm::vec3>& a,
                                   const std::vector<glm::vec3>& b,
                                   const std::vector<float>& radius)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    const size_t count = std::min({ a.size(), b.size(), radius.size(),
                                    sizeof(row.bone) / sizeof(row.bone[0]) });
    for (size_t i = 0; i < count; ++i) {
        MicroBoneRow& br = row.bone[i];
        br.a[0] = a[i].x;
        br.a[1] = a[i].y;
        br.a[2] = a[i].z;
        br.a[3] = radius[i]; // per-bone capsule radius (for microBoneAt)
        br.b[0] = b[i].x;
        br.b[1] = b[i].y;
        br.b[2] = b[i].z;
    }
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

void MicroSvoPass::setWorldAnimation(size_t worldIndex, glm::vec3 offset, glm::mat3 R,
                                     glm::vec3 pivot, bool active)
{
    if (!m_worldTable.mapped || worldIndex >= size_t(m_worldCount))
        return;
    auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
    MicroWorldRow& row = rows[worldIndex];
    row.anim[0] = offset.x;
    row.anim[1] = offset.y;
    row.anim[2] = offset.z;
    row.anim[3] = active ? 1.0f : 0.0f;
    row.arot0[0] = R[0][0]; row.arot0[1] = R[0][1]; row.arot0[2] = R[0][2];
    row.arot1[0] = R[1][0]; row.arot1[1] = R[1][1]; row.arot1[2] = R[1][2];
    row.arot2[0] = R[2][0]; row.arot2[1] = R[2][1]; row.arot2[2] = R[2][2];
    row.apivot[0] = pivot.x;
    row.apivot[1] = pivot.y;
    row.apivot[2] = pivot.z;
    vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
}

size_t MicroSvoPass::gpuBytes() const
{
    return m_grid.bytes + m_childBase.bytes + m_payload.bytes + m_handles.bytes +
           m_bricks.bytes + m_chunkInfo.bytes + m_field.bytes + m_meshVertsBuf.bytes;
}

bool MicroSvoPass::setMesh(size_t worldIndex, const std::vector<glm::vec4>& verts)
{
    if (!m_ctx || !m_set || worldIndex >= size_t(m_worldCount) || verts.empty())
        return false;
    if (m_meshVerts.size() <= worldIndex)
        m_meshVerts.resize(worldIndex + 1);
    m_meshVerts[worldIndex] = verts;

    // Concatenate and re-upload. This runs at LOAD time only (a reload re-runs
    // the whole thing), so copying a few MB is cheaper than the bookkeeping a
    // per-slice upload would need to relocate on growth.
    std::vector<glm::vec4> flat;
    for (const auto& v : m_meshVerts)
        flat.insert(flat.end(), v.begin(), v.end());
    if (!uploadSsbo(m_meshVertsBuf, flat.data(), flat.size() * sizeof(glm::vec4))) {
        spdlog::error("micro: failed to upload the mesh vertices for world {}", worldIndex);
        return false;
    }
    VkDescriptorBufferInfo info { m_meshVertsBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 11, 0,
                             1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &info,
                             nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);

    // Flip the world to RASTER-OWNED. The tracer skips it (so the rest pose is
    // no longer drawn on top of the posed mesh - the object would be its own
    // ghost) and the field sampler stops un-boning its points.
    m_meshWorldCount = 0;
    if (m_worldTable.mapped) {
        auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
        rows[worldIndex].hi[3] = 1.0f;
        for (size_t i = 0; i < m_worldCount; ++i)
            m_meshWorldCount += rows[i].hi[3] > 0.5f ? 1 : 0;
        vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
    }
    spdlog::info("micro: world {} raster-owned, {} vertices, {} mesh world(s)", worldIndex,
                 flat.size(), m_meshWorldCount);
    return true;
}

bool MicroSvoPass::init(const Context& ctx)
{
    m_ctx = &ctx;
    VkDevice dev = ctx.device();

    // Binding 0 is intentionally absent (an unwritten binding invalidates the
    // whole set on this driver). b[] count is load-bearing: one more entry than
    // this overflows the array with no error anywhere.
    //
    // Every binding is declared for ALL THREE stages: the raster mesh pipeline
    // shares this set with the compute tracer (that is the point of one shared
    // layout), and a per-stage split would let the two paths drift apart.
    const VkShaderStageFlags gfx = VkShaderStageFlags(
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayoutBinding b[24] = {};
    int n = 0;
    // 1-6: the micro worlds' own six pools (separate from the world's SVO set).
    for (uint32_t i = 1; i < 7; ++i)
        b[n++] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, gfx, nullptr };
    // 7: per-world scale table.
    b[n++] = { 7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    // 10: the dense signed-distance fields (one byte per cell, all worlds
    // concatenated). Separate from the bricks so the field is continuous
    // across brick boundaries - see MicroWorld::field.
    b[n++] = { 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, gfx, nullptr };
    // 11: the raster mesh worlds' vertices, vec4(rest.xyz, bone + 1), all
    // objects concatenated (the vertex shader indexes it with gl_VertexIndex,
    // so a world's slice is just the draw's firstVertex).
    b[n++] = { 11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, gfx, nullptr };
    // 8/9: terrain height texture + coarse object volume (declared by the micro
    // shader itself; shared_model includes reference them by name).
    b[n++] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    b[n++] = { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    // 12/13/14: the main backend's hdr/gpos/gnorm, read-modify-write. gpos is
    // the depth reference; the others are the composited output.
    b[n++] = { 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    b[n++] = { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    b[n++] = { 14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    // atlas sampler + table (shared detailAlbedo; same binding numbers as the
    // other backends so common_base.glsl's declaration is reused as-is).
    b[n++] = { 22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    b[n++] = { 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    // explicit point lights (shared with the other backends)
    b[n++] = { 25, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    // coarse irradiance volume (same binding as the other sets)
    b[n++] = { 26, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, gfx, nullptr };
    // Workstream B: sun camera UBO (28) + sun depth storage image (29).
    b[n++] = { 28, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, gfx, nullptr };
    b[n++] = { 29, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };
    // 41: the PRE-MESH gpos snapshot, read-only. The raster mesh fragment uses
    // it as its world-depth reference; it must not read the live gpos, which
    // the same draw writes (a read/write hazard made the world/micro verdict
    // depend on fragment completion order).
    b[n++] = { 41, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, gfx, nullptr };

    VkDescriptorSetLayoutCreateInfo li { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = n;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_setLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc { gfx, 0, sizeof(RaymarchPush) };
    VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_layout) != VK_SUCCESS)
        return false;

    auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/micro_overlay.comp.spv");
    if (spirv.empty())
        return false;
    VkShaderModuleCreateInfo mci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = spirv.size();
    mci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule mod;
    vkCreateShaderModule(dev, &mci, nullptr, &mod);
    VkComputePipelineCreateInfo cpi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.layout = m_layout;
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    const VkResult r =
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (r != VK_SUCCESS) {
        spdlog::critical("micro: pipeline failed ({})", int(r));
        return false;
    }

    // ---- the raster MESH pipeline ----------------------------------------
    // Same layout, same descriptor set and the same push block as the tracer -
    // that is the point of one shared set. It is DEPTH-ONLY: the fragment stage
    // writes hdr/gpos/gnorm with imageStore exactly like the compute path, so
    // the world/micro depth comparison and every downstream consumer (fur
    // cards, SSAO, SSR, post) cannot tell which path drew the object.
    //
    // The one attachment is this pass's own depth, which exists so a mesh
    // occludes ITSELF and the other mesh objects; the main backend's depth is
    // never touched. Linear depth (zc / 200 m) in the vertex shader keeps the
    // two trivially in step, and the depth test is what orders two overlapping
    // objects' gpos writes.
    {
        auto makeModule = [&](const char* file, VkShaderModule* out) {
            auto spirv = loadSpirv(std::string(VOXELFORGE_SHADER_DIR) + "/" + file);
            if (spirv.empty())
                return false;
            VkShaderModuleCreateInfo smci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            smci.codeSize = spirv.size();
            smci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
            return vkCreateShaderModule(dev, &smci, nullptr, out) == VK_SUCCESS;
        };
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        if (!makeModule("micro_mesh.vert.spv", &vs) ||
            !makeModule("micro_mesh.frag.spv", &fs)) {
            spdlog::critical("micro: mesh shader module load failed");
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        // No vertex input at all: the whole object is one storage buffer and
        // the vertex shader pulls its record with gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo vi {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };
        VkPipelineInputAssemblyStateCreateInfo ia {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
        };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
        };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
        };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // CULLING IS ON, and it is the fix for the flicker this pass had.
        //
        // The mesh writes its G-buffer through `imageStore` from the FRAGMENT
        // stage. A closed shell drawn with culling OFF covers ~40 % of the
        // silhouette's pixels TWICE — the near wall and the far wall — and
        // where the depth test is a near-tie (interpolation noise, not
        // geometry) `LESS` does not settle which fragment survives. Both
        // execute, both store, and the last store wins, so the frame differs
        // run to run. Measured 2026-11-13: two renders of ONE binary with a
        // frozen pose differed on 4,536 px at the flicker camera, and
        // `layout(early_fragment_tests)` is inert on this driver so nothing
        // orders the stores. Culling removes the second writer, and the depth
        // test keeps early-Z: one fragment per pixel, nothing to race.
        //
        // It is only SAFE because `loadMicroMesh` now orients the winding:
        // the shipped alpaca arrived with 7,008 of its 26,897 shared edges
        // (26.1 %) traversed the same way by both neighbours, so a quarter of
        // the body faced inward and culling BACK deleted the outer wall
        // there. The falsifiable half, measured in one session on one binary:
        // the two cull conventions left near-DISJOINT residuals (4,536 px
        // unculled; 2,475 px with the shipping front face; 693 px with the
        // opposite one; Jaccard of the two residual masks 0.002, 5 px of
        // 3,163 overlapping). Every remaining ambiguous pixel was decided by
        // WHICH WALL SURVIVED the cull — the geometric definition of two
        // opposite walls contending, not of a barrier race.
        //
        // VF_MESH_CULL stays as a diagnostic: =0 disables culling (the
        // pre-fix A/B), =2 culls with the OPPOSITE front face (the
        // "orientation is inverted" arm). Neither is a configuration.
        const char* meshCull = std::getenv("VF_MESH_CULL");
        const bool cullOff = meshCull && meshCull[0] == '0';
        const bool flipFront = meshCull && meshCull[0] == '2';
        rs.cullMode = cullOff ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = flipFront ? VK_FRONT_FACE_CLOCKWISE
                                 : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
        };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds {
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
        };
        // The mesh is a closed, consistently outward-wound surface (the loader
        // certifies both), so backface culling plus the depth test leave
        // exactly one fragment per pixel. Keep both enabled: without the depth
        // test the last-drawn triangle wins every pixel regardless of distance.
        ds.depthTestEnable = VK_TRUE;
        // NO depth write here. The depth buffer is owned by the PREPASS below:
        // this pass tests EQUAL against it, so a pixel can only be written by
        // the fragment that is exactly the nearest one. That is the difference
        // between "the nearest wins" and "the last store wins", and it matters
        // because `imageStore` is not suppressed by the depth test.
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
        ds.depthBoundsTestEnable = VK_FALSE;
        ds.stencilTestEnable = VK_FALSE;
        // No colour attachments: the fragment stage's imageStore is the
        // output. Vulkan allows a depth-only dynamic-rendering pass.
        VkPipelineRenderingCreateInfo prc {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
        };
        prc.colorAttachmentCount = 0;
        prc.pColorAttachmentFormats = nullptr;
        prc.depthAttachmentFormat = kMicroMeshDepthFormat;

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
        };
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gpi {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        };
        gpi.pNext = &prc;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vi;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds;
        gpi.pDynamicState = &dy;
        gpi.layout = m_layout;
        const VkResult mr =
            vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &m_meshPipeline);

        // ---- the DEPTH PREPASS ------------------------------------------
        // The vertex stage ALONE. With no fragment shader there is no
        // `imageStore`, so this pass can only write depth — and two contending
        // fragments then resolve by the rasterizer's min-depth rule, which does
        // not depend on the order they arrive in. That is the one property the
        // shaded pass cannot offer, because a shader-side image write is not
        // suppressed by a late depth test (`layout(early_fragment_tests)` is
        // inert on this driver). The shaded pass then tests EQUAL against this
        // pass's depth, so exactly one fragment per pixel survives to write the
        // G-buffer.
        VkPipelineDepthStencilStateCreateInfo dsd = ds;
        dsd.depthWriteEnable = VK_TRUE;
        dsd.depthCompareOp = VK_COMPARE_OP_LESS;
        VkGraphicsPipelineCreateInfo gpd = gpi;
        gpd.stageCount = 1; // vertex only - no fragment stage at all
        gpd.pDepthStencilState = &dsd;
        const VkResult dr = vkCreateGraphicsPipelines(
            dev, VK_NULL_HANDLE, 1, &gpd, nullptr, &m_meshDepthPipeline);
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
        if (mr != VK_SUCCESS || dr != VK_SUCCESS) {
            spdlog::critical("micro: mesh pipeline failed (shaded {}, prepass {})",
                             int(mr), int(dr));
            return false;
        }

        // ---- SUN-SPACE DEPTH pipeline (Workstream B) --------------------
        // Renders posed meshes from the sun into an R32F colour attachment
        // (separate from the storage-bound copy) with a D32 attachment for
        // nearest-wins. The fragment writes a COLOUR, so the depth test
        // suppresses far fragments - deterministic, no imageStore race.
        VkShaderModule sunVS = VK_NULL_HANDLE, sunFS = VK_NULL_HANDLE;
        if (!makeModule("micro_sun.vert.spv", &sunVS) ||
            !makeModule("micro_sun.frag.spv", &sunFS)) {
            spdlog::critical("micro: sun shadow shader module load failed");
            return false;
        }
        VkPipelineShaderStageCreateInfo ss[2] = {};
        ss[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = sunVS; ss[0].pName = "main";
        ss[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = sunFS; ss[1].pName = "main";
        VkFormat sunColorFormats[] = { VK_FORMAT_R32_SFLOAT };
        VkPipelineRenderingCreateInfo sunPRC {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        sunPRC.colorAttachmentCount = 1;
        sunPRC.pColorAttachmentFormats = sunColorFormats;
        sunPRC.depthAttachmentFormat = kMicroMeshDepthFormat;
        VkPipelineDepthStencilStateCreateInfo sunDS = ds;
        sunDS.depthWriteEnable = VK_TRUE;
        sunDS.depthCompareOp = VK_COMPARE_OP_LESS;
        VkGraphicsPipelineCreateInfo spi {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        spi.pNext = &sunPRC;
        spi.stageCount = 2; spi.pStages = ss;
        spi.pVertexInputState = &vi;
        spi.pInputAssemblyState = &ia;
        spi.pViewportState = &vp;
        spi.pRasterizationState = &rs; // shares cull mode with the mesh pass
        spi.pMultisampleState = &ms;
        spi.pDepthStencilState = &sunDS;
        spi.pDynamicState = &dy;
        spi.layout = m_layout;
        const VkResult sr = vkCreateGraphicsPipelines(
            dev, VK_NULL_HANDLE, 1, &spi, nullptr, &m_sunPipeline);
        vkDestroyShaderModule(dev, sunVS, nullptr);
        vkDestroyShaderModule(dev, sunFS, nullptr);
        if (sr != VK_SUCCESS) {
            spdlog::critical("micro: sun depth pipeline failed ({})", int(sr));
            return false;
        }
    }

    // Pool sizes cover every binding above: STORAGE_IMAGE 8/9/12/13/14 (5),
    // STORAGE_BUFFER 1-6 + 10 + 11 (8), UNIFORM_BUFFER 7/23/25 (3),
    // COMBINED_IMAGE_SAMPLER 22/26 (2).
    VkDescriptorPoolSize sizes[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 },
                                       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 },
                                       { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(dev, &ai, &m_set) != VK_SUCCESS)
        return false;

    // world table: 16 rows of 64 B + the 16 B count word
    m_worldTable = makeBuffer(ctx, sizeof(MicroWorldRow) * kMicroMaxWorlds + 16,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_worldTable.buf || !m_worldTable.mapped)
        return false;
    memset(m_worldTable.mapped, 0, sizeof(MicroWorldRow) * kMicroMaxWorlds + 16);
    VkDescriptorBufferInfo wtInfo { m_worldTable.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wwt { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 7, 0,
                               1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &wtInfo,
                               nullptr };
    vkUpdateDescriptorSets(dev, 1, &wwt, 0, nullptr);

    // Binding 11 must be WRITTEN even when no layer uses a mesh: an unwritten
    // binding in a declared layout is what silently invalidates the whole set
    // on this driver, and that failure looks like "the pass draws nothing".
    // Sixteen zero bytes is a legal empty buffer; no mesh world is drawn, so no
    // shader invocation ever reads it.
    {
        const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
        if (!uploadSsbo(m_meshVertsBuf, zero, sizeof(zero)))
            return false;
    }
    VkDescriptorBufferInfo meshInfo { m_meshVertsBuf.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wmesh { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 11,
                                 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
                                 &meshInfo, nullptr };
    vkUpdateDescriptorSets(dev, 1, &wmesh, 0, nullptr);

    // Workstream B: bindings 28 (sun UBO) and 29 (sun depth) must be WRITTEN at
    // init even with the feature off, or the whole set is invalid on this
    // driver. Placeholder 1x1 R32F + identity viewProj with enabled=0.
    {
        struct SunShadow_t { glm::mat4 vp; glm::vec4 params; } sun0 {};
        sun0.vp = glm::mat4(1.f);
        sun0.params = glm::vec4(0.f);
        m_sunUbo = makeBuffer(ctx, sizeof(sun0), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
        if (!m_sunUbo.buf || !m_sunUbo.mapped)
            return false;
        memcpy(m_sunUbo.mapped, &sun0, sizeof(sun0));
        VkDescriptorBufferInfo bi { m_sunUbo.buf, 0, sizeof(sun0) };
        VkWriteDescriptorSet w28 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                   m_set, 28, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                   nullptr, &bi, nullptr };
        vkUpdateDescriptorSets(dev, 1, &w28, 0, nullptr);

        m_sunDepth = makeImage2D(*m_ctx, 1, 1, VK_FORMAT_R32_SFLOAT,
                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!m_sunDepth.img)
            return false;
        m_sunDepthExtent = { 1, 1 };
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            vf::transitionImage(cmd, m_sunDepth.img, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        });
        VkDescriptorImageInfo di { VK_NULL_HANDLE, m_sunDepth.view,
                                   VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w29 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                                   m_set, 29, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   &di, nullptr, nullptr };
        vkUpdateDescriptorSets(dev, 1, &w29, 0, nullptr);
    }
    return true;
}

void MicroSvoPass::updateDescriptors(const Image3D& hdrImage, const Image3D& gposImage,
                                     const Image3D& gnormImage)
{
    if (!m_set || !m_ctx)
        return;
    // The mesh depth attachment is created lazily in record() from the render
    // extent (m_hdr is not sized yet during init), so nothing to do here.
    // The SNAPSHOT is created there too, but the copy source is this image.
    m_gposImage = gposImage.img;
    VkDescriptorImageInfo ii { VK_NULL_HANDLE, hdrImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo gi { VK_NULL_HANDLE, gposImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo ni { VK_NULL_HANDLE, gnormImage.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo hi { VK_NULL_HANDLE, m_heightView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo oi { VK_NULL_HANDLE, m_objVolView, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[6] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 8, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 9, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, nullptr, nullptr };
    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 12, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr };
    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 13, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 14, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ni, nullptr, nullptr };
    // 41 starts out naming the LIVE gpos: an unwritten binding in a declared
    // layout invalidates the whole set on this driver, and a frame with no mesh
    // world never copies a snapshot. ensureMeshScene() rebinds it to the
    // snapshot the moment one exists.
    w[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 41, 0, 1,
             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &gi, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 6, w, 0, nullptr);
}

bool MicroSvoPass::ensureMeshDepth(uint32_t slot, uint32_t w, uint32_t h)
{
    slot %= kMeshSlots;
    Image3D& depth = m_meshDepth[slot];
    VkExtent2D& extent = m_meshDepthExtent[slot];
    if (!m_ctx) { spdlog::error("ensureMeshDepth: no ctx"); return false; }
    if (w == 0 || h == 0) { spdlog::error("ensureMeshDepth: zero extent {}x{}", w, h); return false; }
    if (depth.img && extent.width == w && extent.height == h)
        return true;
    destroyImage3D(*m_ctx, depth);
    depth = makeImage2D(*m_ctx, w, h, kMicroMeshDepthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!depth.img) {
        spdlog::critical("micro: mesh depth image {}x{} failed", w, h);
        extent = { 0, 0 };
        return false;
    }
    extent = { w, h };
    // Leave it in DEPTH_ATTACHMENT_OPTIMAL once, here (the same shape the fur
    // accumulators use): the frame path then only needs the write-after-write
    // dependency between frames, never a layout transition.
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    });
    if (!ok)
        spdlog::critical("micro: mesh depth transition failed");
    else
        spdlog::info("micro: mesh depth slot {} created {}x{}", slot, w, h);
    return ok;
}

bool MicroSvoPass::ensureSunDepth()
{
    if (!m_ctx)
        return false;
    const uint32_t R = kSunDepthRes;
    if (m_sunDepthRT.img && m_sunDepthAttach.img && m_sunDepth.img &&
        m_sunDepthExtent.width == R)
        return true;
    if (m_sunDepthRT.img) destroyImage3D(*m_ctx, m_sunDepthRT);
    if (m_sunDepthAttach.img) destroyImage3D(*m_ctx, m_sunDepthAttach);
    if (m_sunDepth.img) destroyImage3D(*m_ctx, m_sunDepth);
    // Render target: colour attachment + copy SOURCE (never bound as storage).
    m_sunDepthRT = makeImage2D(*m_ctx, R, R, VK_FORMAT_R32_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    m_sunDepthAttach = makeImage2D(*m_ctx, R, R, kMicroMeshDepthFormat,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
    // Storage copy: bound at 29, never a colour attachment.
    m_sunDepth = makeImage2D(*m_ctx, R, R, VK_FORMAT_R32_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!m_sunDepthRT.img || !m_sunDepthAttach.img || !m_sunDepth.img) {
        spdlog::critical("micro: sun depth targets {}x{} failed", R, R);
        return false;
    }
    m_sunDepthExtent = { R, R };
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_sunDepthRT.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        vf::transitionImage(cmd, m_sunDepthAttach.img, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        vf::transitionImage(cmd, m_sunDepth.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    });
    // Rebind the shading read (29) to the real storage copy.
    VkDescriptorImageInfo di { VK_NULL_HANDLE, m_sunDepth.view,
                               VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w29 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                               m_set, 29, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               &di, nullptr, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w29, 0, nullptr);
    return true;
}

bool MicroSvoPass::ensureMeshScene(uint32_t w, uint32_t h)
{
    if (!m_ctx) { spdlog::error("ensureMeshScene: no ctx"); return false; }
    if (w == 0 || h == 0) { spdlog::error("ensureMeshScene: zero extent {}x{}", w, h); return false; }
    if (m_meshScene.img && m_meshSceneExtent.width == w &&
        m_meshSceneExtent.height == h)
        return true;
    destroyImage3D(*m_ctx, m_meshScene);
    m_meshScene = makeImage2D(*m_ctx, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_STORAGE_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!m_meshScene.img) {
        spdlog::critical("micro: mesh scene snapshot {}x{} failed", w, h);
        m_meshSceneExtent = { 0, 0 };
        return false;
    }
    m_meshSceneExtent = { w, h };
    // Leave it in GENERAL once: a storage-image read AND a transfer
    // destination both accept GENERAL, so the frame only needs access
    // dependencies around the copy, never a layout transition.
    const bool ok = m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        vf::transitionImage(cmd, m_meshScene.img, VK_IMAGE_ASPECT_COLOR_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    });
    if (!ok) {
        spdlog::critical("micro: mesh scene snapshot transition failed");
        return false;
    }
    // The view changed, so the read-only binding must name the new image.
    VkDescriptorImageInfo si { VK_NULL_HANDLE, m_meshScene.view,
                               VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w41 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                               m_set, 41, 0, 1,
                               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &si, nullptr,
                               nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w41, 0, nullptr);
    spdlog::info("micro: mesh scene snapshot {}x{} created, binding 41 rebound",
                 w, h);
    return true;
}

void MicroSvoPass::setTexAtlas(VkImageView view, VkSampler sampler, VkBuffer tableUbo)
{
    if (!m_ctx || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
        tableUbo == VK_NULL_HANDLE)
        return;
    const VkDescriptorImageInfo img { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorBufferInfo buf { tableUbo, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {};
    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 22, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img, nullptr, nullptr };
    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 23, 0, 1,
             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &buf, nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 2, w, 0, nullptr);
}

void MicroSvoPass::setIrrVolView(VkImageView view)
{
    if (!m_ctx || view == VK_NULL_HANDLE)
        return;
    m_irrView = view;
    if (!m_irrSampler) {
        VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        if (vkCreateSampler(m_ctx->device(), &sci, nullptr, &m_irrSampler) != VK_SUCCESS) {
            spdlog::error("micro: irradiance sampler creation failed (binding 26 unbound)");
            m_irrSampler = VK_NULL_HANDLE;
            return;
        }
    }
    const VkDescriptorImageInfo img { m_irrSampler, view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 26, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &img, nullptr,
                             nullptr };
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
}

void MicroSvoPass::setLights(const vf::voxel::LightUBO& lights)
{
    if (!m_ctx)
        return;
    if (!m_lightsBuf.buf)
        m_lightsBuf = makeBuffer(*m_ctx, sizeof(vf::voxel::LightUBO),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST, true);
    if (!m_lightsBuf.buf || !m_lightsBuf.mapped)
        return;
    memcpy(m_lightsBuf.mapped, &lights, sizeof(lights));
    if (m_set) {
        VkDescriptorBufferInfo info { m_lightsBuf.buf, 0, sizeof(lights) };
        VkWriteDescriptorSet w { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, 25, 0,
                                 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &info,
                                 nullptr };
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
}

void MicroSvoPass::setSunShadow(const glm::mat4& viewProj, bool enabled)
{
    if (!m_ctx || !m_sunUbo.mapped)
        return;
    struct SunShadow_t { glm::mat4 vp; glm::vec4 params; } sun0 {};
    sun0.vp = viewProj;
    sun0.params = glm::vec4(enabled ? 1.f : 0.f);
    memcpy(m_sunUbo.mapped, &sun0, sizeof(sun0));
    vmaFlushAllocation(m_ctx->allocator(), m_sunUbo.alloc, 0, VK_WHOLE_SIZE);
    m_sunShadowOn = enabled && m_sunPipeline != VK_NULL_HANDLE;
}

void MicroSvoPass::record(VkCommandBuffer cmd, const RaymarchPush& push,
                          uint32_t frameSlot)
{
    // No micro worlds loaded -> nothing to composite. This is the whole cost of
    // the feature when it is off, and it guarantees the main render path is
    // byte-identical to before.
    if (m_worldCount == 0 || !m_pipeline || !m_set)
        return;
    // Frustum cull: skip the pass if no micro world's AABB intersects the view.
    // Also compute a screen-space bounding rectangle so the dispatch only
    // covers the pixels that can possibly contain a micro object (a full-screen
    // dispatch wastes ~95% of its threads on empty sky for a small vase).
    int rectMinX = (int)push.a.z, rectMinY = (int)push.a.w;
    int rectMaxX = 0, rectMaxY = 0;
    bool anyVisible = false;
    {
        const float tanHalf = push.a.x;
        const float aspect = push.a.y;
        const uint32_t W = (uint32_t)push.a.z;
        const uint32_t H = (uint32_t)push.a.w;
        auto* rows = reinterpret_cast<MicroWorldRow*>(m_worldTable.mapped);
        // THE RENDERED OBJECT IS NOT AT ITS BAKED AABB. The behaviour (the
        // alpaca's walk) and the live Move/Rotate previews are applied when the
        // FIELD IS SAMPLED, so the object moves while its box stays put. This
        // rectangle CLIPS the dispatch and `anyVisible` can skip the pass
        // entirely, so both must follow the same forward chain the shader
        // applies in microShadeToBaked - rotation preview, then the behaviour
        // (pivot rotation, then offset), then the Move translation.
        //
        // Measured: the box alone put a walking alpaca outside the rectangle,
        // and those pixels were simply never dispatched - the animal went fully
        // or partly transparent while it walked, while the fur pass (which
        // already mirrored this chain) still drew its coat.
        auto rowMat = [](const float* a, const float* b2, const float* c2) {
            glm::mat3 M;
            M[0] = glm::vec3(a[0], a[1], a[2]);
            M[1] = glm::vec3(b2[0], b2[1], b2[2]);
            M[2] = glm::vec3(c2[0], c2[1], c2[2]);
            return M;
        };
        auto posedCentre = [&rowMat](const MicroWorldRow& r) {
            glm::vec3 c(0.5f * (r.lo[0] + r.hi[0]), 0.5f * (r.lo[1] + r.hi[1]),
                        0.5f * (r.lo[2] + r.hi[2]));
            if (r.pivot[3] > 0.5f) {
                const glm::vec3 pv(r.pivot[0], r.pivot[1], r.pivot[2]);
                c = pv + rowMat(r.rot0, r.rot1, r.rot2) * (c - pv);
            }
            if (r.anim[3] > 0.5f) {
                const glm::vec3 pv(r.apivot[0], r.apivot[1], r.apivot[2]);
                c = pv + rowMat(r.arot0, r.arot1, r.arot2) * (c - pv) +
                    glm::vec3(r.anim[0], r.anim[1], r.anim[2]);
            }
            if (r.preview[3] > 0.5f)
                c += glm::vec3(r.preview[0], r.preview[1], r.preview[2]);
            return c;
        };
        for (int i = 0; i < m_worldCount; ++i) {
            const MicroWorldRow& r = rows[i];
            glm::vec3 mn(r.lo[0], r.lo[1], r.lo[2]);
            glm::vec3 mx(r.hi[0], r.hi[1], r.hi[2]);
            const glm::vec3 c = posedCentre(r);
            const glm::vec3 d = c - glm::vec3(push.camPos);
            const float zc = glm::dot(d, glm::vec3(push.camFwd));
            // The rest AABB's half-diagonal, plus the reach of one BONE: a
            // swinging limb carries the surface outside the box it was baked
            // in, and the shader's field answers for it (the field is the rest
            // pose, sampled through the bone's inverse).
            const float radius = 0.5f * glm::length(mx - mn) + 0.30f;
            if (zc < -radius)
                continue; // fully behind the camera
            if (zc > 60.0f + radius)
                continue; // too far (sub-pixel)
            const float xc = glm::dot(d, glm::vec3(push.camRight));
            const float yc = glm::dot(d, glm::vec3(push.camUp));
            const float halfX = tanHalf * aspect * std::max(zc, 0.1f) + radius;
            const float halfY = tanHalf * std::max(zc, 0.1f) + radius;
            if (std::fabs(xc) > halfX || std::fabs(yc) > halfY)
                continue;
            anyVisible = true;
            // Project the bounding sphere's four screen extremes; the NEAREST
            // depth it can reach is used, so a sphere that straddles the camera
            // does not collapse the rectangle. This mirrors the fur pass, so
            // the coat and the body it covers always agree on the screen area.
            const float zn = std::max(zc - radius, 0.1f);
            for (int s = 0; s < 4; ++s) {
                const float px = xc + ((s & 1) ? radius : -radius);
                const float py = yc + ((s & 2) ? radius : -radius);
                const int sx =
                    (int)((px / (tanHalf * aspect * zn) * 0.5f + 0.5f) * float(W));
                const int sy =
                    (int)((-py / (tanHalf * zn) * 0.5f + 0.5f) * float(H));
                rectMinX = std::min(rectMinX, sx - 16);
                rectMinY = std::min(rectMinY, sy - 16);
                rectMaxX = std::max(rectMaxX, sx + 16);
                rectMaxY = std::max(rectMaxY, sy + 16);
            }
        }
        if (!anyVisible)
            return;
        // clamp
        rectMinX = std::max(0, std::min(rectMinX, (int)W - 1));
        rectMinY = std::max(0, std::min(rectMinY, (int)H - 1));
        rectMaxX = std::max(rectMinX + 1, std::min(rectMaxX, (int)W));
        rectMaxY = std::max(rectMinY + 1, std::min(rectMaxY, (int)H));
        if (const char* dbg = std::getenv("VF_TRACE_MICRO_RECT"); dbg && *dbg) {
            for (int i = 0; i < m_worldCount; ++i) {
                const MicroWorldRow& r = rows[i];
                const glm::vec3 c = posedCentre(r);
                const glm::vec3 d = c - glm::vec3(push.camPos);
                const float z = glm::dot(d, glm::vec3(push.camFwd));
                const float x = glm::dot(d, glm::vec3(push.camRight));
                const float y = glm::dot(d, glm::vec3(push.camUp));
                const float zn = std::max(z, 0.1f);
                const int sx = (int)((x / (tanHalf * aspect * zn) * 0.5f + 0.5f) * float(W));
                const int sy = (int)((-y / (tanHalf * zn) * 0.5f + 0.5f) * float(H));
                spdlog::info("microrect world {} baked ({:.2f},{:.2f},{:.2f}) "
                             "anim ({:.2f},{:.2f},{:.2f}) on {} posed ({:.2f},{:.2f},{:.2f}) "
                             "screen ({},{}) z {:.2f} rect [{},{}..{},{}]",
                             i, 0.5f * (r.lo[0] + r.hi[0]), 0.5f * (r.lo[1] + r.hi[1]),
                             0.5f * (r.lo[2] + r.hi[2]), r.anim[0], r.anim[1], r.anim[2],
                             r.anim[3] > 0.5f ? 1 : 0, c.x, c.y, c.z, sx, sy, z,
                             rectMinX, rectMinY, rectMaxX, rectMaxY);
            }
        }
    }
    // Flush the host-written world table before the GPU reads it. makeBuffer
    // allocates with VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    // which on a discrete GPU is host-visible but NOT host-coherent, so a
    // plain memcpy into the mapped range does not reach the device. The
    // symptom is silent and confusing: the dispatch runs and every early-out
    // passes, but `uWorldInfo.x` still reads 0, so the shader returns before
    // tracing anything and the pass renders nothing at all.
    if (m_worldTable.mapped)
        vmaFlushAllocation(m_ctx->allocator(), m_worldTable.alloc, 0, VK_WHOLE_SIZE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0,
                            nullptr);
    RaymarchPush pushData = push;
    pushData.b.z = (float)rectMinX;
    pushData.b.w = (float)rectMinY;
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushData), &pushData);
    vkCmdDispatch(cmd, uint32_t(rectMaxX - rectMinX + 7) / 8,
                  uint32_t(rectMaxY - rectMinY + 7) / 8, 1);

    // CPU-STATE SPLIT for the mesh worlds. A headless render of a MESH object
    // used to differ from run to run at a fixed pose, so the first question is
    // whether the POSE the CPU uploads is identical: hash the per-world bone
    // table here, once per frame. Same hash in two runs + different pixels
    // means the divergence is on the GPU side (pass ordering / the raster
    // path), not in the rig or the clip clock.
    if (const char* dbg = std::getenv("VF_TRACE_MESH_HASH"); dbg && *dbg) {
        static int traceFrame = 0;
        const uint8_t* tb = reinterpret_cast<const uint8_t*>(m_worldTable.mapped);
        const size_t tn = sizeof(MicroWorldRow) * m_worldCount;
        uint64_t h = 1469598103934665603ull;
        for (size_t k = 0; k < tn; ++k) { h ^= tb[k]; h *= 1099511628211ull; }
        spdlog::info("meshhash frame {} table {:016x} bytes {} worlds {} meshWorlds {}",
                     traceFrame++, h, tn, m_worldCount, m_meshWorldCount);
        const auto* rows = reinterpret_cast<const MicroWorldRow*>(m_worldTable.mapped);
        for (size_t i = 0; i < m_worldCount; ++i) {
            spdlog::info("meshhash   world {} anim {:.6f} {:.6f} {:.6f} {:.3f}",
                         i, rows[i].anim[0], rows[i].anim[1], rows[i].anim[2],
                         rows[i].anim[3]);
        }
    }

    // ---- the RASTER MESH worlds ------------------------------------------
    // Everything above traced a field. A world whose geometry is a MESH is
    // skipped by the tracer (microWorldIsMesh) and drawn here instead, into the
    // same G-buffer, so the object is its surface and never the field's
    // rest-pose invention: no hole, no cut, no ghost. The field still supplies
    // every shaded term (material, texture, normal, AO, self-shadow), read at
    // the REST point the vertex stage passes down.
    // The per-frame resources come from the frame loop's OWN in-flight slot:
    // see the kMeshSlots comment in the header for why an attachment that every
    // frame clears cannot be single-buffered while frames overlap.
    const uint32_t meshSlot = frameSlot % kMeshSlots;
    Image3D& meshDepth = m_meshDepth[meshSlot];
    if (m_meshWorldCount > 0 && m_meshPipeline != VK_NULL_HANDLE &&
        ensureMeshDepth(meshSlot, uint32_t(push.a.z), uint32_t(push.a.w)) &&
        ensureMeshScene(uint32_t(push.a.z), uint32_t(push.a.w)) &&
        meshDepth.img && m_meshScene.img &&
        m_meshDepthExtent[meshSlot].width == uint32_t(push.a.z) &&
        m_meshDepthExtent[meshSlot].height == uint32_t(push.a.w) &&
        m_meshSceneExtent.width == uint32_t(push.a.z) &&
        m_meshSceneExtent.height == uint32_t(push.a.w)) {
        // SNAPSHOT the world reference BEFORE any mesh fragment writes.
        // The fragment stage needs gpos (the world's distance along the view
        // ray) but also writes it; reading the image a raster pass writes is a
        // read/write hazard, and a fragment that saw a SIBLING's position got a
        // distance off by up to a pixel footprint - enough to flip the
        // half-voxel boundary test, so a pixel toggled between the world and
        // the coat depending on fragment completion order. A copy of the rect
        // makes the fragment's read a plain, ordered read.
        if (m_gposImage != VK_NULL_HANDLE) {
            {
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                // WRITE: the world's gpos - a COLOUR ATTACHMENT in splat mode
                // (oGPos, location 1) and a storage image in the SVO/tile path
                // - plus the tracer's storage stores. READ: a PREVIOUS frame's
                // mesh fragments reading the snapshot (single-buffered across
                // frames in flight, so overwriting it is a write-after-read).
                mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT |
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd, &di);
            }
            VkImageCopy crop {};
            crop.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            crop.srcOffset = { int32_t(rectMinX), int32_t(rectMinY), 0 };
            crop.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            crop.dstOffset = { int32_t(rectMinX), int32_t(rectMinY), 0 };
            crop.extent = { uint32_t(rectMaxX - rectMinX),
                            uint32_t(rectMaxY - rectMinY), 1 };
            vkCmdCopyImage(cmd, m_gposImage, VK_IMAGE_LAYOUT_GENERAL,
                           m_meshScene.img, VK_IMAGE_LAYOUT_GENERAL, 1, &crop);
            {
                VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd, &di);
            }
        }
        // The caller's barrier only makes the dispatch visible to COMPUTE, so
        // without this the mesh's other reads race the tracer.
        {
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        }
        // No dependency on the previous frame's depth here, and none was ever
        // effective: the same-layout transition this replaced did nothing,
        // because a barrier inside a command buffer orders work inside ITS OWN
        // submission and cannot reach a previous submission at all. The ordering
        // is structural instead — this frame draws into the depth attachment of
        // its own in-flight slot, which the frame loop's fence already proves
        // idle. See kMeshSlots in the header.

        // ---- SUN-SPACE DEPTH pass + copy (Workstream B, default OFF) ------
        // Render the posed meshes from the sun into m_sunDepthRT (a pure colour
        // attachment), then COPY it into m_sunDepth (storage binding 29). The
        // two images are essential: writing a colour attachment that is also
        // bound as a storage image in the same pass is a hazard that drops the
        // writes - the cause of the first attempt's clear-value-only map.
        if (m_sunShadowOn && ensureSunDepth() &&
            m_sunDepthExtent.width == kSunDepthRes) {
            VkRenderingAttachmentInfo sunColor {};
            sunColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            sunColor.imageView = m_sunDepthRT.view;
            sunColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            sunColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            sunColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            sunColor.clearValue.color = { 2.0f, 0.f, 0.f, 0.f }; // far = lit
            VkRenderingAttachmentInfo sunDepthAtt {};
            sunDepthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            sunDepthAtt.imageView = m_sunDepthAttach.view;
            sunDepthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            sunDepthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            sunDepthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            sunDepthAtt.clearValue.depthStencil = { 1.0f, 0 };
            VkRenderingInfo sunRI { VK_STRUCTURE_TYPE_RENDERING_INFO };
            const VkRect2D sunRect { { 0, 0 }, { kSunDepthRes, kSunDepthRes } };
            sunRI.renderArea = sunRect;
            sunRI.layerCount = 1;
            sunRI.colorAttachmentCount = 1;
            sunRI.pColorAttachments = &sunColor;
            sunRI.pDepthAttachment = &sunDepthAtt;
            vkCmdBeginRendering(cmd, &sunRI);
            const VkViewport sunVP { 0.f, 0.f, float(kSunDepthRes), float(kSunDepthRes), 0.f, 1.f };
            vkCmdSetViewport(cmd, 0, 1, &sunVP);
            vkCmdSetScissor(cmd, 0, 1, &sunRect);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sunPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                                    0, 1, &m_set, 0, nullptr);
            uint32_t sunBase = 0;
            for (size_t i = 0; i < m_worldCount; ++i) {
                const size_t nv = i < m_meshVerts.size() ? m_meshVerts[i].size() : 0;
                if (nv == 0) continue;
                RaymarchPush sp = push;
                sp.b.z = float(i);
                vkCmdPushConstants(cmd, m_layout,
                    VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT),
                    0, sizeof(sp), &sp);
                vkCmdDraw(cmd, uint32_t(nv), 1, sunBase, 0);
                sunBase += uint32_t(nv);
            }
            vkCmdEndRendering(cmd);

            // RT: COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL.
            VkImageMemoryBarrier2 rtb { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            rtb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            rtb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            rtb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            rtb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            rtb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            rtb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            rtb.image = m_sunDepthRT.img;
            rtb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            // STORAGE copy: GENERAL -> TRANSFER_DST_OPTIMAL.
            VkImageMemoryBarrier2 dbb = rtb;
            dbb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            dbb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                VK_ACCESS_2_TRANSFER_WRITE_BIT;
            dbb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            dbb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            dbb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            dbb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dbb.image = m_sunDepth.img;
            VkImageMemoryBarrier2 bars[] = { rtb, dbb };
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.imageMemoryBarrierCount = 2;
            di.pImageMemoryBarriers = bars;
            vkCmdPipelineBarrier2(cmd, &di);

            VkImageCopy sunCopy {};
            sunCopy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            sunCopy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            sunCopy.extent = { kSunDepthRes, kSunDepthRes, 1 };
            vkCmdCopyImage(cmd, m_sunDepthRT.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_sunDepth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &sunCopy);

            // Restore layouts: RT -> COLOR_ATTACHMENT_OPTIMAL, copy -> GENERAL.
            VkImageMemoryBarrier2 rb1 = rtb;
            rb1.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            rb1.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            rb1.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            rb1.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            rb1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            rb1.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkImageMemoryBarrier2 rb2 = dbb;
            rb2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            rb2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            rb2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            rb2.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            rb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            rb2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkImageMemoryBarrier2 rbs[] = { rb1, rb2 };
            VkDependencyInfo rdi { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            rdi.imageMemoryBarrierCount = 2;
            rdi.pImageMemoryBarriers = rbs;
            vkCmdPipelineBarrier2(cmd, &rdi);
        }

        const VkRect2D rect { { rectMinX, rectMinY },
                              { uint32_t(rectMaxX - rectMinX),
                                uint32_t(rectMaxY - rectMinY) } };
        VkRenderingAttachmentInfo depth {};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = meshDepth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = { 1.0f, 0 };
        VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = rect;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 0;
        ri.pColorAttachments = nullptr;
        ri.pDepthAttachment = &depth;

        vkCmdBeginRendering(cmd, &ri);
        const VkViewport gvp { 0.0f, 0.0f, float(push.a.z), float(push.a.w), 0.0f, 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &gvp);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        // One draw per world: `firstVertex` is the world's slice of the shared
        // vertex buffer and b.z names the world (the fragment stage needs it
        // for the field lookups). The depth attachment orders the objects, so
        // the order they are drawn in does not change the result.
        auto drawMeshWorlds = [&](VkPipeline pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout,
                                    0, 1, &m_set, 0, nullptr);
            uint32_t meshBase = 0;
            for (size_t i = 0; i < m_worldCount; ++i) {
                const size_t nv =
                    i < m_meshVerts.size() ? m_meshVerts[i].size() : size_t(0);
                if (nv == 0)
                    continue;
                RaymarchPush mp = push;
                mp.b.z = float(i);
                vkCmdPushConstants(cmd, m_layout,
                                   VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT |
                                                      VK_SHADER_STAGE_FRAGMENT_BIT),
                                   0, sizeof(mp), &mp);
                vkCmdDraw(cmd, uint32_t(nv), 1, meshBase, 0);
                meshBase += uint32_t(nv);
            }
        };

        // PREPASS. Depth only - it has no fragment stage, so it cannot write a
        // G-buffer texel even if two fragments cover the same pixel. Its
        // outcome is the nearest depth per pixel, which the rasterizer
        // resolves by comparison, not by arrival order.
        drawMeshWorlds(m_meshDepthPipeline);
        {
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            mb.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            mb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        }
        // SHADED pass, EQUAL against the prepass depth: only the fragment that
        // IS the nearest one survives, so a pixel has one writer and the
        // frame is deterministic even though `imageStore` is not gated by a
        // late depth test.
        drawMeshWorlds(m_meshPipeline);
        vkCmdEndRendering(cmd);

        // The FRAGMENT stage wrote the G-buffer, and the caller's next barrier
        // only names COMPUTE as the source (it was written for the dispatch).
        // Make the fragment writes visible here so the pass is self-contained
        // and the frame files stay agnostic about which path drew a pixel.
        {
            VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo di { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        }
    }
}

void MicroSvoPass::destroy()
{
    if (!m_ctx)
        return;
    VkDevice dev = m_ctx->device();
    destroySsbo(m_grid);
    destroySsbo(m_childBase);
    destroySsbo(m_payload);
    destroySsbo(m_handles);
    destroySsbo(m_bricks);
    destroySsbo(m_chunkInfo);
    destroySsbo(m_field);
    destroySsbo(m_meshVertsBuf);
    if (m_irrSampler)
        vkDestroySampler(dev, m_irrSampler, nullptr);
    if (m_pipeline)
        vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_meshPipeline)
        vkDestroyPipeline(dev, m_meshPipeline, nullptr);
    if (m_meshDepthPipeline)
        vkDestroyPipeline(dev, m_meshDepthPipeline, nullptr);
    if (m_sunPipeline)
        vkDestroyPipeline(dev, m_sunPipeline, nullptr);
    destroyImage3D(*m_ctx, m_sunDepthRT);
    destroyImage3D(*m_ctx, m_sunDepthAttach);
    destroyImage3D(*m_ctx, m_sunDepth);
    m_sunDepthExtent = { 0, 0 };
    destroyBuffer(*m_ctx, m_sunUbo);
    for (int s = 0; s < kMeshSlots; ++s) {
        destroyImage3D(*m_ctx, m_meshDepth[s]);
        m_meshDepthExtent[s] = { 0, 0 };
    }
    destroyImage3D(*m_ctx, m_meshScene);
    m_meshSceneExtent = { 0, 0 };
    m_gposImage = VK_NULL_HANDLE;
    m_meshVerts.clear();
    m_meshWorldCount = 0;
    m_meshPipeline = VK_NULL_HANDLE;
    m_meshDepthPipeline = VK_NULL_HANDLE;
    if (m_layout)
        vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_setLayout)
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_pool)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    destroyBuffer(*m_ctx, m_worldTable);
    destroyBuffer(*m_ctx, m_lightsBuf);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_irrSampler = VK_NULL_HANDLE;
    m_worldCount = 0;
}

} // namespace vf