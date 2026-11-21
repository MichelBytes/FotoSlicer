#pragma once

#include <atomic>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json_fwd.hpp>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>

class Button;
class Label;
class TextInput;
class wxWebView;
class wxWindow;

namespace Slic3r {

class Http;

namespace GUI {

// FotoSlicer: the Models tab, an in-app browser for the free model sites, as Anycubic Slicer Next
// embeds Makeronline. A model a site sends to a slicer ("Open in slicer" links), or a model file the
// browser opens or downloads, is put on the plate.
//
// Search All asks every site at once and shows one list, most liked and downloaded first.
// Printables answers through its public GraphQL API; the other sites are searched in a hidden
// browser, as a visitor would, because they answer only to a browser: their result links are read
// from the page (Thangs: from the JSON its own page asks for, with likes and downloads).
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
    void start_search();
    void search_next_site();
    void on_searcher_loaded(const std::string& url);
    void on_searcher_message(const wxString& message);
    void add_results(const std::string& site, const std::string& items_json, const std::string& error);
    // Searches the sites with a JSON search a plain request may ask: Printables, Makeronline,
    // Thangs and MakerWorld, all at once.
    void search_sites_with_json(const std::string& query);
    void search_json(const std::string& site, Http http, const std::function<nlohmann::json(const nlohmann::json&)>& items_of);
    void run_on_results_page(const std::string& script);

    wxWebView*           m_browser{nullptr};
    std::vector<Button*> m_site_buttons;
    Label*               m_status{nullptr};
    TextInput*           m_search{nullptr};
    wxWebView*           m_searcher{nullptr}; // the hidden browser that runs the sites' own searches
    wxTimer              m_search_timer;      // a site's time limit
    std::string          m_query;
    size_t               m_search_site{0};    // the site searched now; past the end when all are done
    bool                 m_results_ready{false};
    std::vector<std::string> m_waiting_scripts; // for the results page, before it has loaded
    boost::filesystem::path  m_results_page;
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
