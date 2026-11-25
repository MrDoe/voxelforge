#pragma once

// Where the runtime-edit overlay lives. The path is overridable so the test
// scripts keep their edits out of assets/ (VF_OVERLAY_PATH), which is why the
// brush, the stroke writer, the clear path and the frame loop all ask for it
// instead of composing the path themselves.

#include <string>

namespace vf {
namespace app {

std::string overlayPath();

// Move (or copy) the overlay at `path` to `<path>.<tag>-<unix-seconds>`;
// returns the backup path, empty when there was nothing to back up.
std::string backupOverlayFile(const std::string& path, const char* tag, bool move);

} // namespace app
} // namespace vf
