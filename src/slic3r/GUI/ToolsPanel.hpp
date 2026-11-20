#pragma once

#include <optional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "Shortcuts.hpp"

class wxGridSizer;

namespace Slic3r {
namespace GUI {

class ToolButton;

// FotoSlicer: Photoshop's tool bar, docked left of the 3D view. Each button opens the slicer
// tool its Photoshop counterpart stands for, under the same key. Under the tools, the swatches
// are the project's filaments: a click on one prints the selected parts with it.
class ToolsPanel : public wxPanel
{
public:
    explicit ToolsPanel(wxWindow* parent);

    void sys_color_changed();
    void msw_rescale();

private:
    void on_idle();
    void rebuild_swatches();

    std::vector<ToolButton*> m_tools;
    wxGridSizer*             m_swatches{nullptr};
    std::vector<std::string> m_colours;      // the filament colours the swatches show
    std::optional<Shortcut>  m_active_tool;  // the open tool's shortcut, highlighted
};

} // namespace GUI
} // namespace Slic3r
