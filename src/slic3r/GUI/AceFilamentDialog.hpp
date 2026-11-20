#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/AnycubicLan.hpp"

class Button;
class Label;
class TextInput;
class wxFlexGridSizer;
class wxWindow;

namespace Slic3r {
namespace GUI {

// Reads the spools in an Anycubic ACE unit from the printer over the local network and puts
// them in the sidebar's filament list: material, colour, and how much is left on each spool.
class AceFilamentDialog : public DPIDialog
{
public:
    explicit AceFilamentDialog(wxWindow* parent);
    ~AceFilamentDialog() override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void start_reading();
    void show_result(const AnycubicLan::Result& result, const wxString& host);
    void show_slots();
    void apply();

    TextInput*                        m_host{nullptr};
    Button*                           m_read{nullptr};
    Label*                            m_status{nullptr};
    wxFlexGridSizer*                  m_slot_rows{nullptr};
    Button*                           m_use{nullptr};
    std::vector<AnycubicLan::AceSlot> m_slots;
    // Cleared when the dialog closes, so a read still running drops its answer.
    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace GUI
} // namespace Slic3r
