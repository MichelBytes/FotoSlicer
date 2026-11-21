#pragma once

#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>

class Button;
class Label;
class wxWebView;
class wxWindow;

namespace Slic3r {
namespace GUI {

// FotoSlicer: the Models tab, an in-app browser for the free model sites, as Anycubic Slicer Next
// embeds Makeronline. A model a site sends to a slicer ("Open in slicer" links), or a model file the
// browser opens or downloads, is put on the plate.
class ModelsPanel : public wxPanel
{
public:
    explicit ModelsPanel(wxWindow* parent);
    ~ModelsPanel() override;

private:
    void open_site(size_t site);
    void download_and_open(const wxString& url);
    void open_file(const boost::filesystem::path& path);
    void watch_downloads();

    wxWebView*           m_browser{nullptr};
    std::vector<Button*> m_site_buttons;
    Label*               m_status{nullptr};
    wxTimer              m_watch;

    // The browser saves downloads in the user's Downloads folder: model files that appear there
    // while this tab is open are opened once their size stops changing.
    boost::filesystem::path                       m_downloads;
    std::time_t                                   m_since{0};
    std::set<std::string>                         m_seen;
    std::map<std::string, unsigned long long>     m_sizes;
    bool                                          m_opening{false};
    // Cleared when the panel is destroyed, so a download still running drops its answer.
    std::shared_ptr<std::atomic<bool>>            m_alive;
};

} // namespace GUI
} // namespace Slic3r
