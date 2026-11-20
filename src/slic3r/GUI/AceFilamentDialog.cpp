#include "AceFilamentDialog.hpp"

#include <map>
#include <string>
#include <thread>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/window.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char* HOST_KEY = "anycubic_lan_host";

// A spool's colour, drawn by hand so the dark theme never recolours it.
class Swatch : public wxWindow
{
public:
    Swatch(wxWindow* parent, const std::string& colour)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(26, 26)), wxBORDER_NONE)
        , m_colour(colour.empty() ? wxNullColour : wxColour(colour))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#ACACAC")), 1));
            dc.SetBrush(m_colour.IsOk() ? wxBrush(m_colour) : *wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(wxRect(GetClientSize()).Deflate(1), FromDIP(4));
        });
    }

private:
    wxColour m_colour;
};

bool http_fetch(const std::string& method, const std::string& url, std::string& body, std::string& error)
{
    bool ok   = false;
    auto http = method == "POST" ? Http::post(url) : Http::get(url);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string answer, unsigned) {
            body = std::move(answer);
            ok   = true;
        })
        .on_error([&](std::string, std::string reason, unsigned status) { error = status != 0 ? "HTTP " + std::to_string(status) : reason; })
        .perform_sync();
    return ok;
}

wxString error_text(AnycubicLan::Error error, const wxString& host)
{
    using AnycubicLan::Error;
    switch (error) {
    case Error::None: return {};
    case Error::Unreachable:
        // TRN %1% is the printer's IP address.
        return format_wxstr(_L("No printer answered at %1%. Check the address shown on the printer's network screen, and that this computer is on the same network."), host);
    case Error::NotAPrinter:
        // TRN %1% is the IP address the user entered.
        return format_wxstr(_L("The device at %1% did not answer like an Anycubic printer."), host);
    case Error::CloudMode: return _L("The printer is in cloud mode. Turn on LAN Mode in the printer's network settings, then try again.");
    case Error::NotOffered: return _L("This printer's firmware does not offer the LAN connection. Update the printer's firmware, then try again.");
    case Error::Refused:
    case Error::BrokerRefused: return _L("The printer refused the connection. Turn LAN Mode off and on again on the printer, then try again.");
    case Error::Unreadable: return _L("The printer's answer could not be read.");
    case Error::NoAnswer: return _L("The printer did not report its ACE unit in time. Try again.");
    case Error::NoAce: return _L("The printer reports no ACE unit attached.");
    }
    return {};
}

// The filament_id of the preset that best fits a material the unit reports: an Anycubic preset
// named for it, then one of that type, then a generic one. Unknown otherwise, which sends the
// sync to its generic fallback.
std::string filament_id_for(const std::string& material)
{
    const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    const Preset*           best      = nullptr;
    int                     best_rank = 0;
    for (const Preset& preset : filaments) {
        if (!preset.is_compatible || !preset.is_system || preset.filament_id.empty() || filaments.get_preset_base(preset) != &preset)
            continue;
        const bool same_type = boost::iequals(preset.config.opt_string("filament_type", 0u), material);
        const int  rank      = boost::istarts_with(preset.name, "Anycubic " + material + " @")       ? 3 :
                               same_type && boost::starts_with(preset.name, "Anycubic ")           ? 2 :
                               same_type && boost::starts_with(preset.name, "Generic ")            ? 1 : 0;
        if (rank > best_rank) {
            best      = &preset;
            best_rank = rank;
        }
    }
    return best != nullptr ? best->filament_id : std::string(UNKNOWN_FILAMENT_ID);
}

} // namespace

AceFilamentDialog::AceFilamentDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("ACE Filament"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* intro = new Label(this, _L("Reads the spools in your ACE unit from the printer over your local network. The printer must be in LAN Mode."));
    intro->Wrap(FromDIP(420));
    sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto* host_row = new wxBoxSizer(wxHORIZONTAL);
    host_row->Add(new Label(this, _L("Printer IP address")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_host = new TextInput(this, from_u8(wxGetApp().app_config->get(HOST_KEY)), "", "", wxDefaultPosition, wxSize(FromDIP(160), -1),
                           wxTE_PROCESS_ENTER);
    m_host->GetTextCtrl()->SetHint("192.168.1.50");
    m_host->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start_reading(); });
    host_row->Add(m_host, 1, wxALIGN_CENTER_VERTICAL);
    m_read = new Button(this, _L("Read"));
    m_read->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_read->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_reading(); });
    host_row->Add(m_read, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    sizer->Add(host_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_status = new Label(this, "");
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_slot_rows = new wxFlexGridSizer(4, FromDIP(8), FromDIP(16));
    m_slot_rows->AddGrowableCol(2);
    sizer->Add(m_slot_rows, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // TRN Buttons of the ACE Filament dialog: put the spools read from the unit in the filament list.
    auto* buttons = new DialogButtons(this, {L("Use These Filaments"), "Cancel"}, _L("Use These Filaments"));
    m_use         = buttons->GetButtonFromLabel(_L("Use These Filaments"));
    m_use->Enable(false);
    m_use->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(12));

    SetSizerAndFit(sizer);
    SetMinSize(GetSize());
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    if (!m_host->GetTextCtrl()->GetValue().IsEmpty())
        CallAfter([this]() { start_reading(); });
}

AceFilamentDialog::~AceFilamentDialog() { m_alive->store(false); }

bool AceFilamentDialog::is_for_selected_printer()
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle != nullptr && boost::starts_with(bundle->printers.get_edited_preset().config.opt_string("printer_model"), "Anycubic");
}

void AceFilamentDialog::start_reading()
{
    std::string host = into_u8(m_host->GetTextCtrl()->GetValue());
    boost::algorithm::trim(host);
    if (host.empty() || !m_read->IsEnabled())
        return;
    wxGetApp().app_config->set(HOST_KEY, host);
    m_read->Enable(false);
    m_use->Enable(false);
    m_status->SetLabel(_L("Reading the ACE unit..."));
    Layout();

    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    std::thread([this, alive, host]() {
        AnycubicLan::Result result = AnycubicLan::read_ace(host, http_fetch);
        wxGetApp().CallAfter([this, alive, answer = std::move(result), host]() {
            if (alive->load())
                show_result(answer, from_u8(host));
        });
    }).detach();
}

void AceFilamentDialog::show_result(const AnycubicLan::Result& result, const wxString& host)
{
    m_read->Enable(true);
    m_slots = result.slots;
    const wxString error = error_text(result.error, host);
    m_status->SetLabel(error.IsEmpty() ? from_u8(result.session.model_name) : error);
    m_status->Wrap(FromDIP(420));
    show_slots();
}

void AceFilamentDialog::show_slots()
{
    m_slot_rows->Clear(true);
    bool several_units = false;
    for (const auto& slot : m_slots)
        several_units |= slot.box != m_slots.front().box;
    bool any_filament = false;
    for (const auto& slot : m_slots) {
        any_filament |= !slot.material.empty();
        m_slot_rows->Add(new Swatch(this, slot.material.empty() ? std::string() : slot.color), 0, wxALIGN_CENTER_VERTICAL);
        // TRN %1% is the ACE unit's number, %2% the slot's number in it.
        m_slot_rows->Add(new Label(this, several_units ? format_wxstr(_L("ACE %1%, slot %2%"), slot.box + 1, slot.index + 1) :
                                                         // TRN %1% is a slot's number in the ACE unit.
                                                         format_wxstr(_L("Slot %1%"), slot.index + 1)),
                         0, wxALIGN_CENTER_VERTICAL);
        m_slot_rows->Add(new Label(this, slot.material.empty() ? _L("Empty") : from_u8(slot.material)), 0, wxALIGN_CENTER_VERTICAL);
        wxString amount;
        if (!slot.material.empty())
            // TRN %1% is the share of filament left on the spool, such as 80%.
            amount = slot.percent_left < 0 ? _L("Amount left unknown") : format_wxstr(_L("%1% left"), std::to_string(slot.percent_left) + "%");
        auto* amount_label = new Label(this, amount);
        if (slot.percent_left < 0 && !slot.material.empty())
            amount_label->SetToolTip(_L("The ACE unit knows how much is left only on Anycubic spools with an RFID tag."));
        m_slot_rows->Add(amount_label, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
    }
    m_use->Enable(any_filament);
    wxGetApp().UpdateDlgDarkUI(this);
    GetSizer()->SetSizeHints(this);
    Fit();
    Layout();
    Refresh();
}

void AceFilamentDialog::apply()
{
    std::map<int, DynamicPrintConfig> trays;
    for (const auto& slot : m_slots) {
        if (slot.material.empty())
            continue;
        DynamicPrintConfig tray;
        tray.set_key_value("filament_id", new ConfigOptionStrings{filament_id_for(slot.material)});
        tray.set_key_value("filament_type", new ConfigOptionStrings{slot.material});
        tray.set_key_value("tray_name", new ConfigOptionStrings{std::string(1, char('A' + slot.box)) + std::to_string(slot.index + 1)});
        tray.set_key_value("ams_id", new ConfigOptionStrings{std::to_string(slot.box)});
        tray.set_key_value("slot_id", new ConfigOptionStrings{std::to_string(slot.index)});
        tray.set_key_value("filament_colour", new ConfigOptionStrings{slot.color.empty() ? std::string("#CECECE") : slot.color});
        tray.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
        tray.set_key_value("filament_colour_type", new ConfigOptionStrings{"0"});
        tray.set_key_value("filament_exist", new ConfigOptionBools{true});
        tray.set_key_value("filament_is_support", new ConfigOptionBools{false});
        // Keyed like build_filament_ams_list's trays on the main extruder, in unit then slot
        // order, so every unit's slots follow on: a second ACE gives filaments 5 to 8.
        trays.emplace(0x10000 + slot.box * 0x100 + slot.index, std::move(tray));
    }
    wxString unmatched;
    if (wxGetApp().plater()->sidebar().load_filament_trays(std::move(trays), unmatched) == 0) {
        MessageDialog(this, _L("There are no compatible filaments, and sync is not performed."), _L("ACE Filament"), wxOK).ShowModal();
        return;
    }
    if (!unmatched.IsEmpty())
        MessageDialog(this, _L("There are some unknown or incompatible filaments mapped to generic preset.") + unmatched, _L("ACE Filament"), wxOK)
            .ShowModal();
    EndModal(wxID_OK);
}

void AceFilamentDialog::on_dpi_changed(const wxRect&)
{
    m_host->Rescale();
    m_read->Rescale();
    show_slots();
}

} // namespace GUI
} // namespace Slic3r
