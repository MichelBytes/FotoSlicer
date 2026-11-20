#pragma once

#include <string>
#include <vector>

#include <wx/panel.h>

class Button;
class Label;
class wxWindow;

namespace Slic3r {
namespace GUI {

class FilamentList;
class HistoryList;

// FotoSlicer: the panels docked on the right, as Photoshop stacks Swatches, Layers and History.
// They follow the project when idle; the Layers panel is the object list, moved out of the sidebar.

// The project's filaments: colour, material and, after an ACE sync, how much is left on each spool.
// A click on one prints the selected parts with it.
class FilamentPanel : public wxPanel
{
public:
    explicit FilamentPanel(wxWindow* parent);
    void sys_color_changed();
    void msw_rescale();

private:
    void on_idle();

    Label*        m_source{nullptr};
    Button*       m_sync{nullptr};
    FilamentList* m_list{nullptr};
};

// Every step that can be undone, oldest first, and those that can be redone, greyed. A click
// goes back, or forward, to the state right after that step.
class HistoryPanel : public wxPanel
{
public:
    explicit HistoryPanel(wxWindow* parent);
    void sys_color_changed();
    void msw_rescale();

private:
    void on_idle();

    HistoryList* m_list{nullptr};
};

} // namespace GUI
} // namespace Slic3r
