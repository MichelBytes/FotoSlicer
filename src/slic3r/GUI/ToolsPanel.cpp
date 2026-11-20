#include "ToolsPanel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/window.h>

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "Gizmos/GLGizmoBase.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr int TOOL_ICON_PX = 26;

wxColour panel_colour() { return StateColor::darkModeColorFor(wxColour("#F8F8F8")); }
wxColour hover_colour() { return StateColor::darkModeColorFor(wxColour("#E4E4E4")); }
wxColour active_colour() { return wxColour(47, 111, 214); }

struct ToolInfo
{
    Shortcut    shortcut;
    const char* icon;
    const char* name;
};

// Photoshop's order: move and transform, then the shaping tools, the brushes, type and the ruler.
// A null icon is a gap between groups.
const ToolInfo TOOLS[] = {
    {Shortcut::GizmoMove, "toolbar_move", L("Move Tool")},
    {Shortcut::GizmoRotate, "toolbar_rotate", L("Rotate Tool")},
    {Shortcut::GizmoScale, "toolbar_scale", L("Scale Tool")},
    {Shortcut::GizmoMove, nullptr, nullptr},
    {Shortcut::GizmoFlatten, "toolbar_flatten", L("Place on Face Tool")},
    {Shortcut::GizmoCut, "toolbar_cut", L("Cut Tool")},
    {Shortcut::GizmoMeshBoolean, "toolbar_meshboolean", L("Combine Shapes Tool")},
    {Shortcut::GizmoMove, nullptr, nullptr},
    {Shortcut::GizmoMmuSegmentation, "mmu_segmentation", L("Color Brush Tool")},
    {Shortcut::GizmoFdmSupports, "toolbar_support", L("Support Brush Tool")},
    {Shortcut::GizmoSeam, "toolbar_seam", L("Seam Brush Tool")},
    {Shortcut::GizmoFuzzySkin, "toolbar_fuzzy_skin_paint", L("Texture Brush Tool")},
    {Shortcut::GizmoMove, nullptr, nullptr},
    {Shortcut::GizmoEmboss, "toolbar_text", L("Type Tool")},
    {Shortcut::GizmoMeasure, "toolbar_measure", L("Ruler Tool")},
};

// The tool open in the 3D view, if any.
std::optional<Shortcut> open_tool()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->get_view3D_canvas3D() == nullptr)
        return std::nullopt;
    const GLGizmoBase* gizmo = plater->get_view3D_canvas3D()->get_gizmos_manager().get_current();
    return gizmo != nullptr ? gizmo->shortcut() : std::nullopt;
}

} // namespace

class ToolButton : public wxWindow
{
public:
    ToolButton(wxWindow* parent, const ToolInfo& tool)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(40, 36)), wxBORDER_NONE), m_tool(tool)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(GetSize());
        // TRN %1% is a tool's name, such as "Move Tool". The key that opens it follows in brackets.
        SetToolTip(from_u8(wxGetApp().shortcuts().with_key(_u8L(m_tool.name), m_tool.shortcut)));
        load_icon();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hover = true;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hover = false;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            event.Skip();
            // Opening a tool moves the focus and may switch tabs: not on the mouse handler's stack.
            wxGetApp().CallAfter([shortcut = m_tool.shortcut]() {
                if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr)
                    frame->open_tool(shortcut);
            });
        });
    }

    Shortcut shortcut() const { return m_tool.shortcut; }

    void set_active(bool active)
    {
        if (m_active != active) {
            m_active = active;
            Refresh();
        }
    }

    void load_icon()
    {
        // Photoshop's tool icons are light grey on the dark panel; the 3D tools' own are accent coloured.
        m_icon = create_scaled_bitmap(m_tool.icon, this, TOOL_ICON_PX, false, wxGetApp().dark_mode() ? "#D4D4D4" : "#3C3C3C");
        SetMinSize(FromDIP(wxSize(40, 36)));
        Refresh();
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        if (m_active || m_hover) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_active ? active_colour() : hover_colour()));
            dc.DrawRoundedRectangle(wxRect(GetClientSize()).Deflate(FromDIP(2)), FromDIP(4));
        }
        const wxBitmap& icon = m_icon;
        if (icon.IsOk()) {
            const wxSize size = icon.GetLogicalSize();
            dc.DrawBitmap(icon, (GetClientSize().x - size.x) / 2, (GetClientSize().y - size.y) / 2, true);
        }
    }

    ToolInfo       m_tool;
    wxBitmap       m_icon;
    bool           m_hover{false};
    bool           m_active{false};
};

// One filament's colour; a click gives the filament to the selected parts.
class FilamentSwatch : public wxWindow
{
public:
    FilamentSwatch(wxWindow* parent, int filament, const std::string& colour)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(15, 15)), wxBORDER_NONE), m_colour(colour)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        // TRN %1% is a filament's number in the filament list.
        SetToolTip(format_wxstr(_L("Filament %1%: click to print the selected parts with it"), filament + 1));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#ACACAC")), 1));
            dc.SetBrush(m_colour.IsOk() ? wxBrush(m_colour) : *wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(wxRect(GetClientSize()));
        });
        Bind(wxEVT_LEFT_UP, [filament](wxMouseEvent& event) {
            event.Skip();
            wxGetApp().CallAfter([filament]() {
                if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
                    list->set_extruder_for_selected_items(filament + 1);
            });
        });
    }

private:
    wxColour m_colour;
};

ToolsPanel::ToolsPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(panel_colour());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(FromDIP(6));
    for (const ToolInfo& tool : TOOLS) {
        if (tool.icon == nullptr) {
            sizer->AddSpacer(FromDIP(10));
            continue;
        }
        auto* button = new ToolButton(this, tool);
        m_tools.push_back(button);
        sizer->Add(button, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(4));
    }
    sizer->AddSpacer(FromDIP(14));
    m_swatches = new wxGridSizer(2, FromDIP(3), FromDIP(3));
    sizer->Add(m_swatches, 0, wxALIGN_CENTER_HORIZONTAL);
    sizer->AddStretchSpacer(1);
    SetSizer(sizer);

    // The filament list and the open tool change from everywhere; follow them when idle.
    Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
        on_idle();
        event.Skip();
    });
}

void ToolsPanel::on_idle()
{
    if (!IsShownOnScreen())
        return;
    const std::optional<Shortcut> active = open_tool();
    if (active != m_active_tool) {
        m_active_tool = active;
        for (ToolButton* button : m_tools)
            button->set_active(active.has_value() && button->shortcut() == *active);
    }
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    std::vector<std::string> colours = plater->get_extruder_colors_from_plater_config();
    if (colours != m_colours) {
        m_colours = std::move(colours);
        rebuild_swatches();
    }
}

void ToolsPanel::rebuild_swatches()
{
    m_swatches->Clear(true);
    for (std::size_t i = 0; i < m_colours.size(); ++i)
        m_swatches->Add(new FilamentSwatch(this, int(i), m_colours[i]));
    Layout();
    Refresh();
}

void ToolsPanel::sys_color_changed()
{
    SetBackgroundColour(panel_colour());
    for (ToolButton* button : m_tools)
        button->load_icon();
    rebuild_swatches();
}

void ToolsPanel::msw_rescale()
{
    for (ToolButton* button : m_tools)
        button->load_icon();
    rebuild_swatches();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
