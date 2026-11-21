#include "ModelsPanel.hpp"

#include <array>
#include <string>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <wx/arrstr.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

struct Site
{
    const char* name; // a brand name, not translated
    const char* url;
};

// Makeronline first: Anycubic's own library, for the printer FotoSlicer is set up for.
const std::array<Site, 5> SITES = {{
    {"Makeronline", "https://makeronline.com/"},
    {"Printables", "https://www.printables.com/"},
    {"Thingiverse", "https://www.thingiverse.com/"},
    {"MakerWorld", "https://makerworld.com/"},
    {"Thangs", "https://thangs.com/"},
}};

// The "open in slicer" links model sites offer: <scheme>open?file=<url>.
const std::array<const char*, 4> SLICER_SCHEMES = {{"orcaslicer://", "prusaslicer://", "bambustudio://", "fotoslicer://"}};

bool is_model_name(const std::string& name)
{
    for (const char* ext : {".stl", ".3mf", ".obj", ".step", ".stp", ".amf", ".zip"})
        if (boost::iends_with(name, ext))
            return true;
    return false;
}

// The last path segment of a URL, decoded.
std::string file_name_of(const std::string& url)
{
    std::string path = url.substr(0, url.find_first_of("?#"));
    return Http::url_decode(path.substr(path.find_last_of('/') + 1));
}

wxColour bar_colour() { return StateColor::darkModeColorFor(wxColour("#F8F8F8")); }

} // namespace

ModelsPanel::ModelsPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
    , m_downloads(into_path(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads)))
    , m_since(std::time(nullptr))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    SetBackgroundColour(bar_colour());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* bar   = new wxBoxSizer(wxHORIZONTAL);
    for (size_t i = 0; i < SITES.size(); ++i) {
        auto* button = new Button(this, SITES[i].name);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { open_site(i); });
        bar->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_site_buttons.push_back(button);
    }
    m_status = new Label(this, _L("Click Download on a model and it goes straight onto your plate."));
    m_status->SetFont(Label::Body_12);
    bar->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    const auto add_tool = [this, bar](const wxString& label, auto action) {
        auto* button = new Button(this, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        bar->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    };
    add_tool(_L("Back"), [this]() { if (m_browser != nullptr && m_browser->CanGoBack()) m_browser->GoBack(); });
    add_tool(_L("Forward"), [this]() { if (m_browser != nullptr && m_browser->CanGoForward()) m_browser->GoForward(); });
    add_tool(_L("Reload"), [this]() { if (m_browser != nullptr) m_browser->Reload(); });
    // Some sign-ins (Google's, for one) refuse embedded browsers: the system browser always works,
    // and its downloads are picked up the same way.
    add_tool(_L("Open in Browser"), [this]() {
        if (m_browser != nullptr)
            wxLaunchDefaultBrowser(m_browser->GetCurrentURL());
    });
    sizer->Add(bar, 0, wxEXPAND | wxALL, FromDIP(8));

    m_browser = WebView::CreateWebView(this, SITES[0].url);
    if (m_browser != nullptr) {
        sizer->Add(m_browser, 1, wxEXPAND);
        m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent& event) {
            const std::string url = into_u8(event.GetURL());
            for (const char* scheme : SLICER_SCHEMES) {
                if (!boost::istarts_with(url, scheme))
                    continue;
                event.Veto();
                const size_t at = url.find("file=");
                if (at != std::string::npos)
                    download_and_open(from_u8(Http::url_decode(url.substr(at + 5, url.find('&', at) - (at + 5)))));
                return;
            }
            if (is_model_name(file_name_of(url))) {
                event.Veto();
                download_and_open(event.GetURL());
            }
        });
        // Sites open models and sign-ins in new windows: keep them in this tab.
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& event) {
            if (is_model_name(file_name_of(into_u8(event.GetURL()))))
                download_and_open(event.GetURL());
            else
                m_browser->LoadURL(event.GetURL());
        });
    } else {
        m_status->SetLabel(_L("The built-in browser is not available. Microsoft Edge WebView2 Runtime is needed."));
    }
    SetSizer(sizer);
    open_site(0);

    m_watch.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { watch_downloads(); });
    m_watch.Start(1500);
}

ModelsPanel::~ModelsPanel() { m_alive->store(false); }

void ModelsPanel::open_site(size_t site)
{
    for (size_t i = 0; i < m_site_buttons.size(); ++i)
        m_site_buttons[i]->SetStyle(i == site ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Window);
    if (m_browser != nullptr)
        m_browser->LoadURL(SITES[site].url);
    else
        wxLaunchDefaultBrowser(SITES[site].url);
}

void ModelsPanel::download_and_open(const wxString& url)
{
    std::string name = sanitize_file_basename(file_name_of(into_u8(url)));
    // TRN %1% is the name of a model file being downloaded.
    m_status->SetLabel(format_wxstr(_L("Downloading %1%..."), name.empty() ? into_u8(url) : name));
    const fs::path folder = fs::temp_directory_path() / "FotoSlicer_models";
    auto           alive  = m_alive;
    Http::get(into_u8(url))
        .timeout_connect(10)
        .timeout_max(600)
        .on_complete([this, alive, folder, name](std::string body, unsigned) mutable {
            // A name without a model extension: 3MF and ZIP files are both ZIP archives; text
            // and binary STL are not.
            if (!is_model_name(name))
                name = (name.empty() ? std::string("model") : name) + (body.compare(0, 2, "PK") == 0 ? ".3mf" : ".stl");
            boost::system::error_code error;
            fs::create_directories(folder, error);
            const fs::path         path = folder / name;
            boost::nowide::ofstream file(path.string(), std::ios::binary);
            file.write(body.data(), std::streamsize(body.size()));
            const bool saved = bool(file);
            file.close();
            wxGetApp().CallAfter([this, alive, path, saved]() {
                if (!alive->load())
                    return;
                if (saved)
                    open_file(path);
                else
                    m_status->SetLabel(_L("The model could not be saved."));
            });
        })
        .on_error([this, alive](std::string, std::string error, unsigned status) {
            wxGetApp().CallAfter([this, alive, error, status]() {
                if (alive->load())
                    // TRN %1% is the reason a download failed, such as "HTTP 404".
                    m_status->SetLabel(format_wxstr(_L("Download failed: %1%"), status != 0 ? "HTTP " + std::to_string(status) : error));
            });
        })
        .perform();
}

void ModelsPanel::open_file(const fs::path& path)
{
    if (m_opening || wxGetApp().mainframe == nullptr || wxGetApp().plater() == nullptr)
        return;
    m_opening = true;
    // TRN %1% is the name of the model file put on the plate.
    m_status->SetLabel(format_wxstr(_L("Opened %1%"), from_path(path.filename())));
    wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREPARE));
    wxArrayString files;
    files.Add(from_path(path));
    wxGetApp().plater()->load_files(files);
    m_opening = false;
}

void ModelsPanel::watch_downloads()
{
    // Only what appears while the tab is open is this tab's download.
    if (!IsShownOnScreen() || m_opening) {
        m_since = std::time(nullptr);
        m_sizes.clear();
        return;
    }
    boost::system::error_code error;
    for (fs::directory_iterator it(m_downloads, error), end; !error && it != end; it.increment(error)) {
        const fs::path&   path = it->path();
        const std::string key  = path.string();
        if (m_seen.count(key) != 0 || !is_model_name(path.filename().string()))
            continue;
        boost::system::error_code stat_error;
        const std::time_t written = fs::last_write_time(path, stat_error);
        if (stat_error || written < m_since) {
            m_seen.insert(key);
            continue;
        }
        const unsigned long long size = fs::file_size(path, stat_error);
        if (stat_error)
            continue;
        const auto known = m_sizes.find(key);
        if (known != m_sizes.end() && known->second == size && size > 0) {
            // The same size two checks in a row: the browser has finished writing it.
            m_seen.insert(key);
            m_sizes.erase(known);
            CallAfter([this, path]() { open_file(path); });
        } else {
            m_sizes[key] = size;
        }
    }
}

} // namespace GUI
} // namespace Slic3r
