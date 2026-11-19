#pragma once
// Scenery resolution: which folder holds the CURRENT world.
//
// A scenery is a self-contained folder under <repo>/scenery/<Name>/ holding
// the world content: world.json, heightmap.png, the layer .vxw files, the
// micro/ tree and the live-edit overlay (runtime_edits.vxw). What stays in
// the shared library (VOXELFORGE_ASSET_DIR, "assets/") is material-identity:
// textures/ and models/ — those are bound per MATERIAL, not per scene.
//
// root() resolution order (first hit wins), memoised on first call:
//   1. an explicit setCurrent()/setCliScenery() override,
//   2. VF_SCENERY (a folder name, or a direct path containing '/'),
//   3. the pointer file <base>/last.json written on the last switch,
//   4. the shipped default (kDefaultScenery) when it exists,
//   5. the first discovered scenery folder,
//   6. the shared asset dir (a pre-migration or broken tree: the callers'
//      own world.json existence checks then fail with their clear message).
#include <string>
#include <vector>

namespace vf {
namespace voxel {
namespace scenery {

// The shipped default scenery, used when nothing selects one.
constexpr const char* kDefaultScenery = "LakesideHamlet";

// <repo>/scenery — the sibling of the shared asset dir.
std::string base();
// Absolute world-root directory of the current scenery (see resolution
// order above). Memoised: a pointer-file edit by another process takes
// effect at the next start, not mid-run.
std::string root();
// Folder name of the current root() (its last path component).
std::string current();
// Subfolders of base() that hold a world.json, sorted by name.
std::vector<std::string> list();
// Adopt a scenery as the current one and persist the pointer. Accepts a
// folder name under base() or a direct path (same grammar as VF_SCENERY).
// False when the target has no world.json.
bool setCurrent(const std::string& nameOrPath);
// Apply a --scenery command-line selection BEFORE the first root() call
// (App member initialisers resolve paths during construction). Does not
// touch the pointer file: a CLI override is one-run, not a switch.
void setCliScenery(const std::string& name);

// Resolve a SHARED-library reference (the manifest's "textures" table names
// e.g. "textures/foo.png") recorded against a manifest that lives in a
// scenery folder: try the manifest dir first (a scenery MAY carry a local
// override), then the shared asset dir (the shipped library). Falls back to
// the manifest-dir candidate so the caller's warning names it.
std::string resolveShared(const std::string& manifestDir, const std::string& file);

} // namespace scenery
} // namespace voxel
} // namespace vf
