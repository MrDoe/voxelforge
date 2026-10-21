// The sidebar's ai section: the AI chat body (ChatUi draws into this pane).
#include "app/app.hpp"

#include "app/ui/gizmo_math.hpp"
#include "app/ui/ui_primitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include <spdlog/spdlog.h>

namespace vf {
namespace app {

void App::drawPanelAI()
{
    sectionHeader("Object authoring via tools");
    auto reloadFn = [this] { requestWorldReload(); };
    // "let the alpaca run" -> the animal is told what to do.
    auto behaviourFn = [this](const std::string& who, const std::string& cmd,
                              float speed) {
        return setMicroBehaviour(who, cmd, speed);
    };
    m_chatUi.drawPanel(m_editable, m_layers, m_hoverHit.hit ? &m_hoverHit : nullptr,
                       m_hasSelection ? &m_selectedHit : nullptr, m_hasSelection,
                       reloadFn, behaviourFn);
}

} // namespace app
} // namespace vf
