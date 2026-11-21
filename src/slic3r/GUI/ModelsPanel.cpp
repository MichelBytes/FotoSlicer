#include "ModelsPanel.hpp"

#include <array>
#include <string>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <wx/arrstr.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <nlohmann/json.hpp>

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
#include "Widgets/TextInput.hpp"
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

// The sites Search All runs in the hidden browser, in order. `url` holds %s for the query; a site
// with `type_query` has no known search address, so the query is typed into its own search box.
struct BrowserSearch
{
    const char* name;
    const char* url;
    const char* link_pattern; // a model page's address, as a JavaScript regular expression
    bool        type_query;
    bool        thangs_api;
};
const std::array<BrowserSearch, 4> BROWSER_SEARCHES = {{
    {"Makeronline", "https://makeronline.com/en/", R"(/model/[^/]+/\d+\.html)", true, false},
    {"Thingiverse", "https://www.thingiverse.com/search?q=%s&type=things&sort=relevant", R"(thing:\d+/?([?#].*)?$)", false, false},
    {"MakerWorld", "https://makerworld.com/en/search/models?keyword=%s", R"(/models/\d+)", false, false},
    {"Thangs", "https://thangs.com/search/%s?scope=all", R"(thangs\.com/(m/\d+|.*3d-model))", false, true},
}};

// A JavaScript string literal, safe inside a <script> element too.
std::string js_string(const std::string& text)
{
    std::string literal = nlohmann::json(text).dump();
    for (size_t at = literal.find("</"); at != std::string::npos; at = literal.find("</", at + 3))
        literal.replace(at, 2, "<\\/");
    return literal;
}

// Fills a script's placeholders; the user's query goes in last, so no later pass can touch it.
std::string fill_script(const char* script, const BrowserSearch& site, const std::string& query)
{
    std::string text = script;
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"PATTERN", js_string(site.link_pattern)},
                                   std::pair<std::string, std::string>{"SITE", js_string(site.name)},
                                   std::pair<std::string, std::string>{"QUERY", js_string(query)}})
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
    return text;
}

std::string replace_all_copy(std::string text, const std::string& from, const std::string& to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// Reads the model links on a results page, waiting for the page's own script to show them.
const char* COLLECT_SCRIPT = R"JS((function () {
  const site = SITE, query = QUERY, pattern = new RegExp(PATTERN);
  const before = window.__fotoslicer_before || new Set();
  const send = (items) => window.wx.postMessage(JSON.stringify({fotoslicer_search: site, query: query, items: items}));
  const collect = () => {
    const found = new Map();
    for (const a of document.querySelectorAll('a[href]')) {
      const url = a.href;
      if (!pattern.test(url) || before.has(url)) continue;
      const entry = found.get(url) || {title: '', url: url, image: ''};
      const img = a.querySelector('img');
      if (img && !entry.image) entry.image = img.currentSrc || img.src || '';
      const text = (a.getAttribute('title') || a.getAttribute('aria-label') || a.textContent || '').replace(/\s+/g, ' ').trim();
      if (text.length > entry.title.length && text.length < 160) entry.title = text;
      if (!entry.title && img && img.alt) entry.title = img.alt;
      found.set(url, entry);
    }
    return Array.from(found.values()).filter(e => e.title || e.image).slice(0, 24);
  };
  let tries = 0;
  const poll = () => {
    const items = collect();
    if (items.length >= 6 || ++tries > 30) {
      // Nothing found: say what page the site showed, for the results page's tooltip.
      if (!items.length) window.wx.postMessage(JSON.stringify({fotoslicer_search: site, query: query, items: [],
        error: (document.title || '').slice(0, 60) + ' | ' + location.href.slice(0, 90) + ' | ' + document.querySelectorAll('a[href]').length + ' links'}));
      else send(items);
    } else setTimeout(poll, 500);
  };
  poll();
})();)JS";

// Types the query into a site's own search box, for a site whose search address is not known.
const char* TYPE_SCRIPT = R"JS((function () {
  const site = SITE, query = QUERY;
  window.__fotoslicer_before = new Set(Array.from(document.querySelectorAll('a[href]'), a => a.href));
  const input = document.querySelector('input[type=search], input[placeholder*="earch" i], input[name*="search" i], input[class*="search" i], input[type=text]');
  if (!input) { window.wx.postMessage(JSON.stringify({fotoslicer_search: site, query: query, items: [], error: 'no search box'})); return; }
  input.focus();
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, query);
  input.dispatchEvent(new Event('input', {bubbles: true}));
  input.dispatchEvent(new Event('change', {bubbles: true}));
  for (const type of ['keydown', 'keypress', 'keyup'])
    input.dispatchEvent(new KeyboardEvent(type, {key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true}));
  const form = input.closest('form');
  if (form) { if (form.requestSubmit) form.requestSubmit(); else form.submit(); }
})();)JS";

// Thangs: the JSON its search page asks for, which has likes and downloads; paid models left out.
const char* THANGS_SCRIPT = R"JS((function () {
  const site = SITE, query = QUERY;
  const send = (message) => window.wx.postMessage(JSON.stringify(Object.assign({fotoslicer_search: site, query: query}, message)));
  fetch('/api/search/v5/search-by-text?searchTerm=' + encodeURIComponent(query) + '&page=0&pageSize=24&pageScope=root',
        {headers: {Accept: 'application/json'}, credentials: 'include'})
    .then(r => r.ok ? r.json() : Promise.reject('HTTP ' + r.status))
    .then(d => send({items: (d.items || [])
      .filter(it => !(it.visibility === 'market-paid' || (it.marketplaceInfo && it.marketplaceInfo.priceInUSD)))
      .map(it => ({title: it.name || '', url: it.modelPageUrl || ('https://thangs.com/m/' + it.modelId),
                   image: it.thumbnailUrl || '', likes: it.likesCount || 0, downloads: it.downloadCount || 0}))}))
    .catch(() => send({fallback: true}));
})();)JS";

const char* PRINTABLES_QUERY = R"GQL(query SearchModels($query: String!, $limit: Int, $offset: Int, $ordering: SearchChoicesEnum) {
  result: searchPrints2(query: $query, limit: $limit, offset: $offset, ordering: $ordering, printType: print) {
    items { id name slug likesCount downloadCount image { filePath } }
  }
})GQL";

// The results page. Every text and address from a site is set through the DOM, never as HTML.
const char* RESULTS_PAGE = R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Search</title><style>
body{margin:0;background:#1f1f1f;color:#e6e6e6;font:13px "Segoe UI",system-ui,sans-serif}
header{padding:14px 18px 6px}h1{margin:0 0 8px;font-size:18px;font-weight:600}
#sites span{display:inline-block;margin:0 6px 6px 0;padding:3px 9px;border-radius:10px;background:#323232;color:#bdbdbd;font-size:12px}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px;padding:8px 18px 24px}
a.card{display:flex;flex-direction:column;background:#2b2b2b;border:1px solid #3a3a3a;border-radius:6px;overflow:hidden;color:inherit;text-decoration:none}
a.card:hover{border-color:#2f6fd6}a.card img{width:100%;aspect-ratio:4/3;object-fit:cover;background:#262626}
.t{padding:7px 9px 2px;font-weight:600;line-height:1.3;height:2.6em;overflow:hidden}
.m{display:flex;justify-content:space-between;padding:4px 9px 8px;color:#a8a8a8;font-size:12px}
.b{color:#fff;background:#2f6fd6;border-radius:3px;padding:0 5px}
</style></head><body><header><h1 id="h"></h1><div id="sites"></div></header><div id="grid"></div><script>
const results = [], status = {};
document.getElementById('h').textContent = TITLE;
const notes = {};
function chips() { const box = document.getElementById('sites'); box.replaceChildren(...Object.keys(status).map(s => { const e = document.createElement('span'); e.textContent = s + ': ' + status[s] + (notes[s] && status[s] === NONE ? ' (' + notes[s] + ')' : ''); return e; })); }
function fsStatus(site, text) { status[site] = text; chips(); }
function known(r) { return r.likes >= 0 || r.downloads >= 0; }
function pop(r) { return Math.log10(1 + Math.max(0, r.downloads || 0) + 10 * Math.max(0, r.likes || 0)); }
function fsAdd(site, items, note) {
  items.forEach((it, i) => { it.site = site; it.pos = i; it.n = items.length; results.push(it); });
  status[site] = items.length ? items.length + ' ' + FOUND : NONE;
  if (note) notes[site] = note;
  chips();
  const top = Math.max(3, ...results.filter(known).map(pop));
  const score = r => known(r) ? pop(r) : top * 0.95 * (1 - r.pos / (r.n + 1));
  const grid = document.getElementById('grid');
  grid.replaceChildren(...results.slice().sort((a, b) => score(b) - score(a)).map(r => {
    const a = document.createElement('a'); a.className = 'card'; a.href = r.url;
    const img = document.createElement('img'); img.loading = 'lazy'; if (r.image) img.src = r.image; a.appendChild(img);
    const t = document.createElement('div'); t.className = 't'; t.textContent = r.title || r.url; a.appendChild(t);
    const m = document.createElement('div'); m.className = 'm';
    const b = document.createElement('span'); b.className = 'b'; b.textContent = r.site; m.appendChild(b);
    const n = document.createElement('span'); n.textContent = known(r) ? '♥ ' + (r.likes || 0) + '  ↓ ' + (r.downloads || 0) : ''; m.appendChild(n);
    a.appendChild(m); return a; }));
}
</script></body></html>)HTML";

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
    m_search = new TextInput(this, "", "", "", wxDefaultPosition, wxSize(FromDIP(220), -1), wxTE_PROCESS_ENTER);
    m_search->GetTextCtrl()->SetHint(_L("Search all sites"));
    m_search->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start_search(); });
    bar->Add(m_search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* search_all = new Button(this, _L("Search All"));
    search_all->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    search_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_search(); });
    bar->Add(search_all, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
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
        // The results page takes results once it has loaded.
        m_browser->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent& event) {
            event.Skip();
            if (m_results_page.empty() || event.GetURL() != file_url_from_path(m_results_page))
                return;
            m_results_ready = true;
            for (const std::string& script : m_waiting_scripts)
                m_browser->RunScript(from_u8(script));
            m_waiting_scripts.clear();
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
    m_search_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
        if (&event.GetTimer() == &m_watch) {
            watch_downloads();
        } else if (m_search_site < BROWSER_SEARCHES.size()) {
            if (m_search_step == 1) {
                // Typed, and the site stayed on its page: read the results that replaced its content.
                m_search_step = 2;
                m_searcher->RunScript(from_u8(fill_script(COLLECT_SCRIPT, BROWSER_SEARCHES[m_search_site], m_query)));
                m_search_timer.StartOnce(20000);
            } else {
                add_results(BROWSER_SEARCHES[m_search_site].name, "[]", into_u8(_L("no answer")));
                ++m_search_site;
                search_next_site();
            }
        }
    });
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

void ModelsPanel::start_search()
{
    std::string query = into_u8(m_search->GetTextCtrl()->GetValue());
    boost::algorithm::trim(query);
    if (query.empty() || m_browser == nullptr)
        return;
    m_query = query;
    // A new results page; what the last search still sends is dropped by its query.
    m_results_page  = boost::filesystem::temp_directory_path() / "FotoSlicer_search.html";
    m_results_ready = false;
    m_waiting_scripts.clear();
    {
        // TRN The heading of the merged search results; %1% is what the user searched for.
        const std::string title = into_u8(format_wxstr(_L("Results for \"%1%\" from every site"), query));
        // TRN Shown after a number of models found on one site, as in "12 found".
        std::string page = replace_all_copy(RESULTS_PAGE, "FOUND", js_string(into_u8(_L("found"))));
        page             = replace_all_copy(page, "NONE", js_string(into_u8(_L("nothing found"))));
        page             = replace_all_copy(page, "TITLE", js_string(title)); // last: it holds the query
        boost::nowide::ofstream file(m_results_page.string(), std::ios::binary);
        file << page;
    }
    m_browser->LoadURL(file_url_from_path(m_results_page));
    for (const char* site : {"Printables", "Makeronline", "Thingiverse", "MakerWorld", "Thangs"})
        run_on_results_page("fsStatus(" + js_string(site) + "," + js_string(into_u8(_L("searching..."))) + ");");
    m_status->SetLabel(_L("Searching every site..."));

    search_printables(query);
    if (m_searcher == nullptr) {
        // A browser placed out of sight runs its pages at full speed, unlike a hidden one.
        m_searcher = WebView::CreateWebView(this, "about:blank");
        if (m_searcher != nullptr) {
            // Full size but outside the panel: sites draw their results only into a real viewport.
            m_searcher->SetSize(-4000, -4000, FromDIP(1280), FromDIP(900));
            m_searcher->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) { on_searcher_loaded(); });
            m_searcher->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& event) { on_searcher_message(event.GetString()); });
            m_searcher->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent&) {});
        }
    }
    m_search_site = 0;
    search_next_site();
}

void ModelsPanel::search_next_site()
{
    m_search_timer.Stop();
    if (m_searcher == nullptr || m_search_site >= BROWSER_SEARCHES.size()) {
        m_search_site = BROWSER_SEARCHES.size();
        m_status->SetLabel(_L("Click a model to open its page, then Download."));
        return;
    }
    const BrowserSearch& site = BROWSER_SEARCHES[m_search_site];
    m_search_step             = 0;
    m_searcher->LoadURL(from_u8(replace_all_copy(site.url, "%s", Http::url_encode(m_query))));
    m_search_timer.StartOnce(25000);
}

void ModelsPanel::on_searcher_loaded()
{
    if (m_search_site >= BROWSER_SEARCHES.size())
        return;
    const BrowserSearch& site = BROWSER_SEARCHES[m_search_site];
    const auto fill = [&](const char* script) { return from_u8(fill_script(script, site, m_query)); };
    if (site.type_query && m_search_step == 0) {
        m_search_step = 1;
        m_searcher->RunScript(fill(TYPE_SCRIPT));
        m_search_timer.StartOnce(3000); // read the results here if the site does not load a new page
    } else if (m_search_step != 2) {
        m_search_step = 2;
        m_searcher->RunScript(fill(site.thangs_api ? THANGS_SCRIPT : COLLECT_SCRIPT));
        m_search_timer.StartOnce(20000);
    }
}

void ModelsPanel::on_searcher_message(const wxString& message)
{
    if (m_search_site >= BROWSER_SEARCHES.size())
        return;
    const BrowserSearch& site = BROWSER_SEARCHES[m_search_site];
    const nlohmann::json answer = nlohmann::json::parse(into_u8(message), nullptr, false);
    if (!answer.is_object() || answer.value("fotoslicer_search", std::string()) != site.name || answer.value("query", std::string()) != m_query)
        return;
    if (answer.value("fallback", false)) {
        // Thangs' JSON was refused: read the links on its results page instead.
        m_searcher->RunScript(from_u8(fill_script(COLLECT_SCRIPT, site, m_query)));
        return;
    }
    const auto items = answer.find("items");
    add_results(site.name, items != answer.end() ? items->dump() : "[]", answer.value("error", std::string()));
    ++m_search_site;
    search_next_site();
}

void ModelsPanel::add_results(const std::string& site, const std::string& items_json, const std::string& error)
{
    // The items come from the sites' pages: only known fields, length-limited text and https
    // addresses go on to the results page.
    nlohmann::json       clean = nlohmann::json::array();
    const nlohmann::json items = nlohmann::json::parse(items_json, nullptr, false);
    if (items.is_array())
        for (const nlohmann::json& item : items) {
            if (!item.is_object())
                continue;
            const auto text = [&](const char* key, size_t limit) {
                const auto it = item.find(key);
                return it != item.end() && it->is_string() ? it->get<std::string>().substr(0, limit) : std::string();
            };
            const std::string url = text("url", 600), image = text("image", 600);
            if (!boost::istarts_with(url, "https://"))
                continue;
            nlohmann::json entry = {{"title", text("title", 160)}, {"url", url}, {"image", boost::istarts_with(image, "https://") ? image : ""}};
            for (const char* key : {"likes", "downloads"})
                if (const auto it = item.find(key); it != item.end() && it->is_number())
                    entry[key] = it->get<double>();
            clean.push_back(std::move(entry));
        }
    run_on_results_page("fsAdd(" + js_string(site) + "," + clean.dump() + "," +
                        (clean.empty() && !error.empty() ? js_string(error) : std::string("''")) + ");");
}

void ModelsPanel::search_printables(const std::string& query)
{
    const nlohmann::json body = {{"query", PRINTABLES_QUERY},
                                 {"variables", {{"query", query}, {"limit", 24}, {"offset", 0}, {"ordering", "best_match"}}}};
    auto alive = m_alive;
    Http::post("https://api.printables.com/graphql/")
        .header("Content-Type", "application/json")
        .header("Accept", "application/json")
        .set_post_body(body.dump())
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([this, alive, query](std::string answer, unsigned) {
            nlohmann::json       items  = nlohmann::json::array();
            const nlohmann::json parsed = nlohmann::json::parse(answer, nullptr, false);
            if (parsed.is_object() && parsed.contains("data") && parsed["data"].is_object() && parsed["data"].contains("result") &&
                parsed["data"]["result"].is_object() && parsed["data"]["result"].contains("items") && parsed["data"]["result"]["items"].is_array())
                for (const nlohmann::json& item : parsed["data"]["result"]["items"]) {
                    if (!item.is_object() || !item.contains("id"))
                        continue;
                    const std::string id   = item["id"].is_string() ? item["id"].get<std::string>() : item["id"].dump();
                    const std::string slug = item.value("slug", std::string());
                    nlohmann::json    entry = {{"title", item.value("name", std::string())},
                                               {"url", "https://www.printables.com/model/" + id + "-" + slug},
                                               {"likes", item.value("likesCount", 0)},
                                               {"downloads", item.value("downloadCount", 0)}};
                    if (item.contains("image") && item["image"].is_object() && item["image"].value("filePath", std::string()) != "")
                        entry["image"] = "https://media.printables.com/" + item["image"]["filePath"].get<std::string>();
                    items.push_back(std::move(entry));
                }
            wxGetApp().CallAfter([this, alive, query, json = items.dump()]() {
                if (alive->load() && query == m_query)
                    add_results("Printables", json, "");
            });
        })
        .on_error([this, alive, query](std::string, std::string error, unsigned status) {
            wxGetApp().CallAfter([this, alive, query, error, status]() {
                if (alive->load() && query == m_query)
                    add_results("Printables", "[]", status != 0 ? "HTTP " + std::to_string(status) : error);
            });
        })
        .perform();
}

void ModelsPanel::run_on_results_page(const std::string& script)
{
    if (m_results_ready && m_browser != nullptr)
        m_browser->RunScript(from_u8(script));
    else
        m_waiting_scripts.push_back(script);
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
