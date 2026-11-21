#pragma once

#include <string>

#include <wx/string.h>

#include "AnycubicLan.hpp"
#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;

// Sends G-code to an Anycubic printer in LAN Mode and starts it, with each filament on the
// ACE slot that holds it. The printer takes the file over HTTP at the address it names on port
// 18910, and starts it on the command of its own MQTT broker (see AnycubicLan.hpp).
class AnycubicLanHost : public PrintHost
{
public:
    explicit AnycubicLanHost(DynamicPrintConfig* config);
    ~AnycubicLanHost() override = default;

    const char* get_name() const override;

    bool                       test(wxString& curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }

private:
    std::string m_host; // the printer's address alone: no scheme, port or path
};

namespace AnycubicLan {

// One request through the app's HTTP client, for the LAN calls.
bool http_fetch(const std::string& method, const std::string& url, std::string& body, std::string& error);

// What to tell the user when a LAN call fails. `detail` is the transport's error text.
wxString error_text(Error error, const std::string& detail, const wxString& host);

// The printer's address as the user typed it, without "http://", a port or a path.
std::string bare_host(const std::string& address);

} // namespace AnycubicLan

} // namespace Slic3r
