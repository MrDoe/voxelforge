// STL/OBJ import into a .vxw layer, shared verbatim by the GUI section and the
// VF_TEST_MESH_IMPORT headless hook (so CI exercises the real import path, not
// a parallel one).
#include "app/app.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::rescanMeshFiles()
{
    m_meshFiles.clear();
    namespace fs = std::filesystem;
    const fs::path assetDir(std::string(VOXELFORGE_ASSET_DIR));
    auto addFile = [&](const fs::path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec)
            return;
        std::string ext = path.extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".stl" && ext != ".obj")
            return;
        std::error_code relEc;
        fs::path rel = fs::relative(path, assetDir, relEc);
        const std::string name = (relEc || rel.empty() ? path.filename() : rel)
                                     .generic_string();
        if (!name.empty() &&
            std::find(m_meshFiles.begin(), m_meshFiles.end(), name) == m_meshFiles.end())
            m_meshFiles.push_back(name);
    };

    for (const fs::path& root : { assetDir / "models", assetDir }) {
        std::error_code rootEc;
        for (fs::directory_iterator it(root, rootEc), end;
             !rootEc && it != end; it.increment(rootEc))
            addFile(it->path());
    }
    std::sort(m_meshFiles.begin(), m_meshFiles.end());
}

void App::useMeshImportLayer(const std::string& file)
{
    if (file.empty())
        return;
    namespace fs = std::filesystem;
    const std::string stem = fs::path(file).stem().string();
    const std::string safe = vf::voxel::EditableWorld::sanitizeLayerName(stem);
    if (safe.empty()) {
        m_meshStatus = "Layer name must contain letters, numbers, '_' or '-'";
        m_meshStatusError = true;
        return;
    }
    std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safe.c_str());

    vf::voxel::WorldFileData data;
    const std::string path = std::string(VOXELFORGE_ASSET_DIR) + "/" + file;
    glm::vec3 pivot(0.f);
    if (!vf::voxel::worldfile::read(path, data) ||
        !vf::voxel::worldfile::recordBottomCenter(data.voxels, data.meta, pivot)) {
        m_meshStatus = "Could not read source layer '" + file + "' for its anchor";
        m_meshStatusError = true;
        return;
    }
    m_meshAnchor = {
        int(std::floor((pivot.x + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL)),
        int(std::floor((pivot.y + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL)),
        int(std::floor((pivot.z + 0.5f * vf::voxel::WORLD) / vf::voxel::VOXEL))
    };
    m_meshStatus = "Anchor set to the untransformed bottom-center of " + file;
    m_meshStatusError = false;
}

void App::prepareMeshImport()
{
    rescanMeshFiles();
    if (m_meshImportPrepared)
        return;

    if (m_meshPath[0] == '\0') {
        const std::string cabin = "models/Forrest_Hunting_Cabin.stl";
        if (std::find(m_meshFiles.begin(), m_meshFiles.end(), cabin) != m_meshFiles.end())
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", cabin.c_str());
        else if (!m_meshFiles.empty())
            std::snprintf(m_meshPath, sizeof(m_meshPath), "%s", m_meshFiles.front().c_str());
    }

    bool usedLayerPivot = false;
    if (m_meshLayerName[0] == '\0') {
        const bool selectedIsReplaceable =
            !m_selectedLayer.empty() && m_selectedLayer != "landscape.vxw" &&
            m_selectedLayer != vf::voxel::EditableWorld::kFileName;
        if (selectedIsReplaceable) {
            useMeshImportLayer(m_selectedLayer);
            usedLayerPivot = m_meshStatus.find("Anchor set") != std::string::npos;
        } else if (std::filesystem::exists(std::string(VOXELFORGE_ASSET_DIR) +
                                            "/hamlet_cabin.vxw")) {
            // The authored hamlet uses the imported cabin layer.  Starting
            // with that target makes a re-import a replacement, not a second
            // object, and writeObjectLayer preserves its manifest pose.
            useMeshImportLayer("hamlet_cabin.vxw");
            usedLayerPivot = m_meshStatus.find("Anchor set") != std::string::npos;
        } else if (!m_meshFiles.empty()) {
            const std::string stem = std::filesystem::path(m_meshFiles.front()).stem().string();
            const std::string safe = vf::voxel::EditableWorld::sanitizeLayerName(stem);
            if (!safe.empty())
                std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safe.c_str());
        }
    }
    // A replacement of an existing layer must use that layer's source pivot;
    // a picked surface voxel is only the fallback for a brand-new layer.
    if (m_hasSelection && !usedLayerPivot)
        m_meshAnchor = m_selectedHit.voxel;
    m_meshImportPrepared = true;
}

bool App::importMeshFromGui()
{
    const std::string path(m_meshPath);
    const std::string name(m_meshLayerName);
    const std::string safeName = vf::voxel::EditableWorld::sanitizeLayerName(name);
    if (safeName.empty()) {
        m_meshStatus = "Enter a layer name (letters, numbers, '_' or '-')";
        m_meshStatusError = true;
        return false;
    }
    if (safeName == "landscape" || safeName == vf::voxel::EditableWorld::kLayerName) {
        m_meshStatus = "Mesh import cannot overwrite the landscape or live AI-edit layer";
        m_meshStatusError = true;
        return false;
    }
    const std::string targetFile = safeName + ".vxw";
    const auto protectedTarget = std::find_if(
        m_worldLayers.begin(), m_worldLayers.end(),
        [&](const vf::voxel::worldfile::WorldLayer& layer) {
            return layer.file == targetFile &&
                   (layer.role == "landscape" || layer.role == "packed");
        });
    if (protectedTarget != m_worldLayers.end()) {
        m_meshStatus = "Mesh import cannot overwrite a landscape or packed layer";
        m_meshStatusError = true;
        return false;
    }

    vf::voxel::MeshImportOptions options;
    options.hasFit = m_meshUseFit;
    options.fitMeters = m_meshFitMeters;
    options.scale = m_meshScale;
    options.rotY = m_meshRotY;
    options.swapYz = m_meshSwapYz;
    options.flip = m_meshFlip;
    options.mat = std::clamp(m_meshMaterial, 0, int(vf::voxel::kPaletteN) - 1);

    std::vector<vf::voxel::VoxelRecord> records;
    vf::voxel::MeshImportStats stats;
    std::string err;
    if (!vf::voxel::convertMeshToRecords(path, options, m_meshSolid,
                                        m_meshAnchor, records, stats, err)) {
        m_meshStatus = err;
        m_meshStatusError = true;
        spdlog::warn("mesh GUI import failed: {}", err);
        return false;
    }
    if (!m_editable.writeObjectLayer(safeName, records)) {
        m_meshStatus = "Could not write the object layer or update world.json";
        m_meshStatusError = true;
        return false;
    }

    m_selectedLayer = safeName + ".vxw";
    std::snprintf(m_meshLayerName, sizeof(m_meshLayerName), "%s", safeName.c_str());
    std::ostringstream out;
    out << "Imported " << stats.triangles << " triangles -> " << safeName
        << ".vxw (" << records.size() << " voxels, "
        << stats.nx << "x" << stats.ny << "x" << stats.nz << " grid)";
    if (stats.clamped > 0)
        out << "; " << stats.clamped << " cells were outside the lattice";
    if (m_meshSolid && stats.leak)
        out << "; warning: no interior volume, mesh is not watertight at this scale";
    if (safeName == "hamlet_cabin")
        out << "; existing manifest pose/orientation preserved";
    m_meshStatus = out.str();
    m_meshStatusError = m_meshSolid && stats.leak;
    m_taaFirstFrame = true;
    requestWorldReload();
    spdlog::info("mesh GUI import: {} ({} voxels)", m_meshStatus, records.size());
    return true;
}

} // namespace app
} // namespace vf
