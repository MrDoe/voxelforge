#pragma once

// Where the runtime-edit overlay lives. The path is overridable so the test
// scripts keep their edits out of assets/ (VF_OVERLAY_PATH), which is why the
// brush, the stroke writer, the clear path and the frame loop all ask for it
// instead of composing the path themselves.

#include <string>

namespace vf {
namespace app {

std::string overlayPath();

} // namespace app
} // namespace vf
