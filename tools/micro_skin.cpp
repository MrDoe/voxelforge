// micro_skin.cpp - REGENERATE a micro mesh's `.skin` side-car from the rig.
//
// WHY THIS EXISTS. A micro object's `.skin` is a per-vertex bone table, and
// there used to be no generator in the tree: the file was authored once by a
// scratch script and then drifted. It drifted in a specific and expensive way:
// the file said "nearest bone segment" while `MicroRig::boneAt` - the rule the
// shader and the fur use - had moved to the CAPSULE test
// `max(0, distToSegment - bone.radius)`. Two rules means two ownerships, so
// the body (skinned from the file) and the coat (bound by `boneAt`) disagreed
// on 20.88 % of vertices, which is exactly the coat detaching from the body it
// is supposed to grow out of.
//
// The rule is now stated in ONE place (`MicroRig::boneAt`), and this tool is
// how the file is derived from it. Re-run it whenever the rig changes (segment
// edit, radius edit, re-import):
//
//   ./build/vf_micro_skin assets/micro/alpaca.vxw assets/models/alpaca_mesh.stl assets/micro/alpaca.skin --up z
//
// The canonical actor asset (`alpaca_mesh.stl`) is Z-up and is the DEFAULT, so
// a bare run is already right; `--up` overrides the axis for any other file.
// The requirement itself lives outside this tool: `meshUp` in world.json and
// the app's loader must agree with it, or the runtime binds the mesh in one
// frame and this tool in another.
//
// FRAMES. The rig's segments are in the OBJECT's frame (metres from the
// layer's min corner), and `loadMicroMesh` normalises the mesh into that same
// frame (it removes the exporter's lattice offset), so a vertex and a segment
// are directly comparable with no correction. The app later shifts the rig by
// the layer's placement and leaves the mesh in the object frame; the capsule
// distances are translation invariant, so both stay in agreement.
//
// The tool is app-free (src/voxel only) and prints the ownership histogram and
// the disagreement against any EXISTING skin, so a regeneration is auditable
// rather than a silent overwrite.
#include "voxel/micro_field.hpp"
#include "voxel/micro_mesh.hpp"
#include "voxel/micro_rig.hpp"
#include "voxel/worldfile.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vf::voxel;

namespace {

uint64_t fileStamp(const std::string& path)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec)
        return 0;
    return uint64_t(t.time_since_epoch().count());
}

std::string stemOf(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path;
    return path.substr(0, dot);
}

// Read an existing skin, for the "what did this change" report. Returns an
// empty vector when the file is absent or malformed.
std::vector<int32_t> readExisting(const std::string& path, size_t verts)
{
    std::ifstream in(path);
    if (!in)
        return {};
    std::string line;
    size_t declared = 0, bones = 0;
    // Skip the leading comment block, then read the counts. A fixed "skip one
    // line" missed files with two headers (the writer below emits two) and
    // silently reported "no existing skin" - the one number that makes a
    // regeneration auditable.
    while (std::getline(in, line))
        if (!line.empty() && line[0] != '#')
            break;
    if (std::sscanf(line.c_str(), "%zu %zu", &declared, &bones) != 2)
        return {};
    if (declared != verts)
        return {};
    std::vector<int32_t> out(verts, -1);
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        long long vi = -1;
        int bi = -1;
        if (std::sscanf(line.c_str(), "%lld %d", &vi, &bi) != 2)
            continue;
        if (vi >= 0 && size_t(vi) < verts)
            out[size_t(vi)] = bi;
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string assets = VOXELFORGE_ASSET_DIR;
    // `--up y|z` names the axis the MESH FILE was authored in. It must match
    // the layer's `meshUp`, because the capsule test compares mesh vertices
    // against the rig's Y-up segments: a Z-up file measured as if it were
    // Y-up binds the animal's nose to its hoof and looks plausible.
    UpAxis up = UpAxis::Y;
    bool upSet = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--up" && i + 1 < argc) {
            const std::string v = argv[++i];
            upSet = true;
            if (v == "z" || v == "Z")
                up = UpAxis::Z;
            else if (v != "y" && v != "Y") {
                std::printf("FAIL --up expects y or z, got '%s'\n", v.c_str());
                return 1;
            }
        } else {
            pos.push_back(a);
        }
    }
    const std::string layerPath =
        pos.size() > 0 ? pos[0] : assets + "/micro/alpaca.vxw";
    const std::string meshPath =
        pos.size() > 1 ? pos[1] : assets + "/models/alpaca_mesh.stl";
    const std::string skinPath =
        pos.size() > 2 ? pos[2] : assets + "/micro/alpaca.skin";
    // Optional: the file to DIFF against (defaults to the output path, which
    // is what a regeneration wants). Point it at a backup to audit a change
    // after the fact.
    const std::string comparePath = pos.size() > 3 ? pos[3] : skinPath;
    // With no --up, the DEFAULT asset decides: the canonical actor mesh is a
    // Z-up export, so a bare run must not bind it as Y-up. Any other file
    // defaults to the engine's own Y-up frame. Always print the resolution -
    // an assumed axis is the one thing about this tool a human cannot see.
    if (!upSet) {
        const bool canonical = meshPath.find("alpaca_mesh.stl") != std::string::npos;
        up = canonical ? UpAxis::Z : UpAxis::Y;
        std::printf("axis       %s (inferred from the mesh path; pass --up to "
                    "override)\n",
                    up == UpAxis::Z ? "z" : "y");
    } else {
        std::printf("axis       %s (--up)\n", up == UpAxis::Z ? "z" : "y");
    }

    // ---- the layer's field: only the rig needs it -------------------------
    WorldFileData wf;
    if (!worldfile::read(layerPath, wf)) {
        std::printf("FAIL cannot read layer %s\n", layerPath.c_str());
        return 1;
    }
    MicroField field;
    std::string err;
    if (!buildMicroField(wf.voxels, wf.meta, field, &err)) {
        std::printf("FAIL field: %s\n", err.c_str());
        return 1;
    }

    // ---- the rig: override > cache > derive, the app's own order ----------
    MicroRig rig;
    const std::string rigPath = stemOf(layerPath) + ".rig";
    const std::string overridePath = rigPath + ".json";
    const uint64_t lsize = std::filesystem::file_size(layerPath);
    const uint64_t lmtime = fileStamp(layerPath);
    const char* rigSource = "";
    if (readMicroRigOverride(overridePath, rig, lsize, lmtime)) {
        rigSource = "override";
    } else if (readMicroRigCache(rigPath, rig, lsize, lmtime)) {
        rigSource = "cache";
    } else if (deriveMicroRig(field, rig)) {
        rigSource = "derived";
        writeMicroRigCache(rigPath, rig, lsize, lmtime);
    }
    if (!rig.valid()) {
        std::printf("FAIL no rig for %s\n", layerPath.c_str());
        return 1;
    }
    std::printf("rig        %s (%s), %zu bones, %d legs\n", rigPath.c_str(),
                rigSource, rig.bones.size(), rig.legCount);
    for (size_t i = 0; i < rig.bones.size(); ++i)
        std::printf("  %2zu %-8s r %.3f m  seg (%.3f %.3f %.3f)->(%.3f %.3f %.3f)\n",
                    i, rig.bones[i].name.c_str(), rig.bones[i].radius,
                    rig.bones[i].a.x, rig.bones[i].a.y, rig.bones[i].a.z,
                    rig.bones[i].b.x, rig.bones[i].b.y, rig.bones[i].b.z);

    // ---- the mesh, normalised into the rig's frame ------------------------
    MicroMesh mesh;
    // No skin on the load: the file being regenerated must not influence the
    // vertices, only the report below.
    if (!loadMicroMesh(meshPath, "", mesh, &err, up)) {
        std::printf("FAIL mesh: %s\n", err.c_str());
        return 1;
    }
    std::printf("mesh       %s, %zu verts (frame offset %.4f %.4f %.4f)\n",
                meshPath.c_str(), mesh.rest.size(), mesh.restOffset.x,
                mesh.restOffset.y, mesh.restOffset.z);

    // ---- the rule: THE capsule test, the same call the shader and fur make --
    std::vector<int32_t> bone(mesh.rest.size(), -1);
    std::vector<size_t> perBone(rig.bones.size(), 0);
    size_t unowned = 0;
    for (size_t vi = 0; vi < mesh.rest.size(); ++vi) {
        const int b = rig.boneAt(mesh.rest[vi]);
        bone[vi] = b;
        if (b < 0 || size_t(b) >= perBone.size())
            ++unowned;
        else
            ++perBone[size_t(b)];
    }
    std::printf("ownership  :");
    for (size_t b = 0; b < perBone.size(); ++b)
        std::printf(" %s:%zu", rig.bones[b].name.c_str(), perBone[b]);
    std::printf(" | unowned %zu\n", unowned);

    // The disagreement with the file on disk (or the file named on the command
    // line): the number that says whether this regeneration changed anything,
    // and the number that must be ZERO once it has been regenerated.
    //
    // The mesh is NON-INDEXED, so one position appears once per triangle
    // corner. Two copies that disagree on a bone leave a seam the surface can
    // crack along - the failure `tests/micro_mesh_check` reports as
    // "position-coincident bone disagreements". A rule that is a pure function
    // of position cannot produce one, which is why the capsule rule replaced
    // the authored table; the count below measures the file.
    const std::vector<int32_t> old = readExisting(comparePath, mesh.rest.size());
    if (!old.empty()) {
        size_t diff = 0;
        for (size_t vi = 0; vi < bone.size(); ++vi)
            if (old[vi] != bone[vi])
                ++diff;
        // One entry per position (0.1 mm buckets), so the scan is O(n log n)
        // instead of the quadratic pairwise test.
        auto keyOf = [](glm::vec3 p) {
            constexpr float q = 1e4f; // 0.1 mm
            return std::array<long long, 3> { llround(p.x * q), llround(p.y * q),
                                              llround(p.z * q) };
        };
        std::map<std::array<long long, 3>, int32_t> firstOld, firstNew;
        size_t coincidentOld = 0, coincidentNew = 0;
        for (size_t vi = 0; vi < bone.size(); ++vi) {
            const auto key = keyOf(mesh.rest[vi]);
            auto it = firstOld.find(key);
            if (it == firstOld.end()) {
                firstOld.emplace(key, old[vi]);
                firstNew.emplace(key, bone[vi]);
            } else {
                if (it->second != old[vi])
                    ++coincidentOld;
                if (firstNew[key] != bone[vi])
                    ++coincidentNew;
            }
        }
        std::printf("old skin   : %zu of %zu verts change bone (%.2f%%)\n", diff,
                    bone.size(),
                    bone.empty() ? 0.0 : 100.0 * double(diff) / double(bone.size()));
        std::printf("             %zu positions; OLD file splits %zu copies across "
                    "bones, the new one %zu\n",
                    firstOld.size(), coincidentOld, coincidentNew);
    }

    // ---- write it ---------------------------------------------------------
    {
        std::ofstream out(skinPath);
        if (!out) {
            std::printf("FAIL cannot write %s\n", skinPath.c_str());
            return 1;
        }
        out << "# skin weights: vertex_index bone_index (nearest CAPSULE: distance "
               "to segment minus the bone's radius)\n";
        out << "# bones:";
        for (const MicroBone& b : rig.bones)
            out << ' ' << b.name;
        out << "\n";
        out << mesh.rest.size() << ' ' << rig.bones.size() << "\n";
        for (size_t vi = 0; vi < bone.size(); ++vi)
            out << vi << ' ' << bone[vi] << "\n";
        if (!out) {
            std::printf("FAIL writing %s\n", skinPath.c_str());
            return 1;
        }
    }
    std::printf("wrote      %s\n", skinPath.c_str());

    // ---- verify the file round-trips through the loader -------------------
    MicroMesh check;
    if (!loadMicroMesh(meshPath, skinPath, check, &err, up)) {
        std::printf("FAIL reload: %s\n", err.c_str());
        return 1;
    }
    size_t mismatch = 0;
    for (size_t vi = 0; vi < bone.size(); ++vi)
        if (check.bone[vi] != bone[vi])
            ++mismatch;
    std::printf("verify     %zu verts, %zu bone mismatches vs the capsule rule\n",
                check.rest.size(), mismatch);
    std::printf("SKIN_AGREEMENT=%.2f%% %s\n",
                100.0 * double(bone.size() - mismatch) / double(bone.size()),
                mismatch == 0 ? "PASS" : "FAIL");
    return mismatch == 0 ? 0 : 1;
}
