#include "PhotoshopPanels.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/vlbox.h>

#include "AceFilamentDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxColour panel_bg() { return StateColor::darkModeColorFor(*wxWHITE); }
wxColour text_fg() { return wxGetApp().dark_mode() ? wxColour(230, 230, 230) : wxColour(38, 46, 48); }
wxColour dim_fg() { return wxGetApp().dark_mode() ? wxColour(150, 150, 150) : wxColour(120, 120, 120); }
wxColour row_line() { return wxGetApp().dark_mode() ? wxColour(42, 42, 42) : wxColour(230, 230, 230); }
wxColour current_bg() { return wxGetApp().dark_mode() ? wxColour(44, 62, 92) : wxColour(227, 236, 250); }
const wxColour ACCENT(47, 111, 214);

// The text before " @" in a preset name: "Anycubic PLA @Anycubic Kobra 4 0.4 nozzle" -> "Anycubic PLA".
wxString short_name(const std::string& preset)
{
    return from_u8(preset.substr(0, preset.find(" @")));
}

// A list of rows painted by hand, in the panels' colours.
class PanelListBase : public wxVListBox
{
public:
    PanelListBase(wxWindow* parent, int row_dip) : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_row_dip(row_dip)
    {
        SetBackgroundColour(panel_bg());
        SetFont(Label::Body_13);
    }

protected:
    wxCoord OnMeasureItem(size_t) const override { return FromDIP(m_row_dip); }
    // The row under the pointer is drawn by the derived class; wx's own selection is not used.
    void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t) const override
    {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(panel_bg()));
        dc.DrawRectangle(rect);
        dc.SetPen(wxPen(row_line()));
        dc.DrawLine(rect.GetBottomLeft(), rect.GetBottomRight());
    }

    int m_row_dip;
};

} // namespace

class FilamentList : public PanelListBase
{
public:
    struct Row
    {
        std::string colour;
        wxString    name;
        int         percent{-1}; // -1: not known
        wxString    slot;        // "ACE slot A1", empty when not from the ACE
        bool operator==(const Row& other) const
        {
            return colour == other.colour && name == other.name && percent == other.percent && slot == other.slot;
        }
    };

    explicit FilamentList(wxWindow* parent) : PanelListBase(parent, 46)
    {
        Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
            const int filament = event.GetSelection();
            SetSelection(wxNOT_FOUND);
            if (filament < 0)
                return;
            wxGetApp().CallAfter([filament]() {
                if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
                    list->set_extruder_for_selected_items(filament + 1);
            });
        });
    }

    void set_rows(std::vector<Row> rows)
    {
        if (rows == m_rows)
            return;
        m_rows = std::move(rows);
        SetItemCount(m_rows.size());
        Refresh();
    }

    void refresh_colours()
    {
        SetBackgroundColour(panel_bg());
        Refresh();
    }

protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override
    {
        const Row& row = m_rows[n];
        const int  pad = FromDIP(8);
        dc.SetFont(GetFont());
        dc.SetTextForeground(dim_fg());
        const wxString number = wxString::Format("%d", int(n) + 1);
        dc.DrawText(number, rect.x + pad, rect.y + (rect.height - dc.GetTextExtent(number).y) / 2 - FromDIP(2));

        const int radius = FromDIP(11);
        const wxPoint centre(rect.x + pad + FromDIP(18) + radius, rect.y + rect.height / 2 - FromDIP(2));
        dc.SetPen(wxPen(wxGetApp().dark_mode() ? wxColour(90, 90, 90) : wxColour(200, 200, 200)));
        dc.SetBrush(wxBrush(wxColour(row.colour)));
        dc.DrawCircle(centre, radius);

        const int text_x = centre.x + radius + FromDIP(10);
        dc.SetTextForeground(text_fg());
        dc.DrawText(row.name, text_x, rect.y + FromDIP(6));
        dc.SetTextForeground(dim_fg());
        dc.SetFont(Label::Body_12);
        dc.DrawText(row.slot.IsEmpty() ? _L("Not synced with the ACE") : row.slot, text_x, rect.y + FromDIP(23));

        // The amount left on the spool; "?" when the ACE cannot tell.
        const wxString amount = row.percent >= 0 ? wxString::Format("%d%%", row.percent) : row.slot.IsEmpty() ? wxString() : wxString("?");
        if (!amount.IsEmpty()) {
            dc.SetFont(GetFont());
            dc.SetTextForeground(text_fg());
            const wxSize size = dc.GetTextExtent(amount);
            dc.DrawText(amount, rect.GetRight() - pad - size.x, rect.y + FromDIP(6));
        }
        if (row.percent >= 0) {
            const wxRect bar(text_x, rect.GetBottom() - FromDIP(6), rect.GetRight() - pad - text_x, FromDIP(3));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxGetApp().dark_mode() ? wxColour(28, 28, 28) : wxColour(225, 225, 225)));
            dc.DrawRectangle(bar);
            dc.SetBrush(wxBrush(row.percent < 15 ? wxColour(214, 92, 47) : ACCENT));
            dc.DrawRectangle(wxRect(bar.x, bar.y, bar.width * row.percent / 100, bar.height));
        }
    }

private:
    std::vector<Row> m_rows;
};

class HistoryList : public PanelListBase
{
public:
    explicit HistoryList(wxWindow* parent) : PanelListBase(parent, 26)
    {
        Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) {
            const int row = event.GetSelection();
            SetSelection(wxNOT_FOUND);
            if (row < 0)
                return;
            // Row 0 is the start; rows 1..done are the steps done, oldest first; then the steps undone.
            const int done = m_done;
            wxGetApp().CallAfter([row, done]() {
                Plater* plater = wxGetApp().plater();
                if (plater == nullptr)
                    return;
                if (row <= done) {
                    const int steps_back = done - row; // back to the state right after step `row`
                    if (steps_back > 0)
                        plater->undo_to(steps_back - 1);
                } else {
                    plater->redo_to(row - done - 1);
                }
            });
        });
    }

    // `done` steps from oldest to newest, then the steps that can be redone, next first.
    void set_steps(std::vector<std::string> done, std::vector<std::string> undone)
    {
        if (done == m_done_names && undone == m_undone_names)
            return;
        m_done_names   = std::move(done);
        m_undone_names = std::move(undone);
        m_done         = int(m_done_names.size());
        SetItemCount(1 + m_done_names.size() + m_undone_names.size());
        ScrollToRow(std::max(0, m_done));
        Refresh();
    }

    void refresh_colours()
    {
        SetBackgroundColour(panel_bg());
        Refresh();
    }

protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override
    {
        const int  row     = int(n);
        const bool current = row == m_done;
        if (current) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(current_bg()));
            dc.DrawRectangle(rect);
        }
        const wxString text = row == 0       ? _L("Start") :
                              row <= m_done  ? from_u8(m_done_names[row - 1]) :
                                               from_u8(m_undone_names[row - m_done - 1]);
        dc.SetFont(GetFont());
        dc.SetTextForeground(row > m_done ? dim_fg() : text_fg());
        dc.DrawText(text, rect.x + FromDIP(12), rect.y + (rect.height - dc.GetTextExtent(text).y) / 2);
    }

private:
    std::vector<std::string> m_done_names;
    std::vector<std::string> m_undone_names;
    int                      m_done{0};
};

FilamentPanel::FilamentPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(panel_bg());
    auto* sizer  = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    m_source     = new Label(this, _L("Not synced with the ACE"));
    m_source->SetFont(Label::Body_12);
    header->Add(m_source, 1, wxALIGN_CENTER_VERTICAL);
    m_sync = new Button(this, _L("Sync"));
    m_sync->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_sync->SetToolTip(_L("Read the spools in the ACE unit and use them as the filament list"));
    m_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        AceFilamentDialog dlg(this);
        dlg.ShowModal();
    });
    header->Add(m_sync, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(8));
    m_list = new FilamentList(this);
    sizer->Add(m_list, 1, wxEXPAND);
    auto* hint = new Label(this, _L("Select a part, then click a filament to print the part with it."));
    hint->SetFont(Label::Body_12);
    hint->SetForegroundColour(dim_fg());
    sizer->Add(hint, 0, wxEXPAND | wxALL, FromDIP(8));
    SetSizer(sizer);
    Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
        on_idle();
        event.Skip();
    });
}

void FilamentPanel::on_idle()
{
    if (!IsShownOnScreen())
        return;
    Plater*       plater = wxGetApp().plater();
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (plater == nullptr || bundle == nullptr)
        return;
    const std::vector<std::string>             colours = plater->get_extruder_colors_from_plater_config();
    const std::vector<AnycubicLan::AceSlot>&    ace     = AceFilamentDialog::used_slots();
    std::vector<FilamentList::Row>             rows;
    for (std::size_t i = 0; i < colours.size(); ++i) {
        FilamentList::Row row;
        row.colour = colours[i];
        row.name   = i < bundle->filament_presets.size() ? short_name(bundle->filament_presets[i]) : wxString();
        // The ACE spool this filament came from, while the filament still has that spool's colour.
        if (i < ace.size() && boost::iequals(ace[i].color, colours[i])) {
            row.percent = ace[i].percent_left;
            // TRN %1% is a slot of the ACE unit, such as A1.
            row.slot = format_wxstr(_L("ACE slot %1%"), std::string(1, char('A' + ace[i].box)) + std::to_string(ace[i].index + 1));
        }
        rows.push_back(std::move(row));
    }
    m_list->set_rows(std::move(rows));
    const wxString source = ace.empty() ? _L("Not synced with the ACE") : from_u8(AceFilamentDialog::used_model());
    if (m_source->GetLabel() != source)
        m_source->SetLabel(source);
}

void FilamentPanel::sys_color_changed()
{
    SetBackgroundColour(panel_bg());
    m_sync->Rescale();
    m_list->refresh_colours();
    Refresh();
}

void FilamentPanel::msw_rescale()
{
    m_sync->Rescale();
    m_list->RefreshAll();
    Layout();
}

HistoryPanel::HistoryPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(panel_bg());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_list      = new HistoryList(this);
    sizer->Add(m_list, 1, wxEXPAND);
    SetSizer(sizer);
    Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
        on_idle();
        event.Skip();
    });
}

void HistoryPanel::on_idle()
{
    Plater* plater = wxGetApp().plater();
    if (!IsShownOnScreen() || plater == nullptr)
        return;
    std::vector<std::string> done, undone;
    const char*              text = nullptr;
    for (int i = 0; plater->undo_redo_string_getter(true, i, &text); ++i)
        done.emplace_back(text);
    std::reverse(done.begin(), done.end());
    for (int i = 0; plater->undo_redo_string_getter(false, i, &text); ++i)
        undone.emplace_back(text);
    m_list->set_steps(std::move(done), std::move(undone));
}

void HistoryPanel::sys_color_changed()
{
    SetBackgroundColour(panel_bg());
    m_list->refresh_colours();
}

void HistoryPanel::msw_rescale() { m_list->RefreshAll(); }

} // namespace GUI
} // namespace Slic3r
