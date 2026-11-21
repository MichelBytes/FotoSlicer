#include "AnycubicLanHost.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {

namespace AnycubicLan {

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

wxString error_text(Error error, const std::string& detail, const wxString& host)
{
    switch (error) {
    case Error::None: return {};
    case Error::Unreachable:
        // curl's CURLE_COULDNT_CONNECT: something is there and turned the connection away, which
        // is what a printer does with LAN Mode off.
        if (detail.find("[Error 7]") != std::string::npos)
            // TRN %1% is the printer's IP address.
            return GUI::format_wxstr(_L("Nothing at %1% accepted the connection. If that is your printer, turn on LAN Mode in its network settings, then try again."), host);
        // TRN %1% is the printer's IP address.
        return GUI::format_wxstr(_L("No printer answered at %1%. Check the address shown on the printer's network screen, and that this computer is on the same network."), host);
    case Error::NotAPrinter:
        // TRN %1% is the IP address the user entered.
        return GUI::format_wxstr(_L("The device at %1% did not answer like an Anycubic printer."), host);
    case Error::CloudMode: return _L("The printer is in cloud mode. Turn on LAN Mode in the printer's network settings, then try again.");
    case Error::NotOffered: return _L("This printer's firmware does not offer the LAN connection. Update the printer's firmware, then try again.");
    case Error::Refused:
    case Error::BrokerRefused: return _L("The printer refused the connection. Turn LAN Mode off and on again on the printer, then try again.");
    case Error::Unreadable: return _L("The printer's answer could not be read.");
    case Error::NoAnswer: return _L("The printer did not answer in time. Try again.");
    case Error::NoAce: return _L("The printer reports no ACE unit attached.");
    }
    return {};
}

std::string bare_host(const std::string& address)
{
    std::string host = address;
    host.erase(0, host.find_first_not_of(" \t"));
    if (const auto scheme = host.find("://"); scheme != std::string::npos)
        host.erase(0, scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    if (const auto port = host.rfind(':'); port != std::string::npos && host.find(']') == std::string::npos)
        host.erase(port);
    host.erase(host.find_last_not_of(" \t") + 1);
    return host;
}

} // namespace AnycubicLan

namespace {

using namespace AnycubicLan;

// How much of a G-code file's start and end is read for its settings comments: OrcaSlicer
// writes the filament used and the configuration, filament colours and types included, last.
constexpr std::size_t HEAD_BYTES = 64 * 1024;
constexpr std::size_t TAIL_BYTES = 2 * 1024 * 1024;

bool read_ends(const boost::filesystem::path& path, std::uintmax_t size, std::string& head, std::string& tail)
{
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    if (!file)
        return false;
    head.resize(std::min<std::uintmax_t>(size, HEAD_BYTES));
    file.read(&head[0], std::streamsize(head.size()));
    tail.clear();
    if (size > HEAD_BYTES) {
        tail.resize(std::min<std::uintmax_t>(size - HEAD_BYTES, TAIL_BYTES));
        file.seekg(std::streamoff(size - tail.size()));
        file.read(&tail[0], std::streamsize(tail.size()));
    }
    return bool(file);
}

} // namespace

AnycubicLanHost::AnycubicLanHost(DynamicPrintConfig* config) : m_host(bare_host(config->opt_string("print_host"))) {}

const char* AnycubicLanHost::get_name() const { return "Anycubic (LAN)"; }

bool AnycubicLanHost::test(wxString& msg) const
{
    const Result result = handshake(m_host, http_fetch);
    if (result.error != Error::None) {
        BOOST_LOG_TRIVIAL(warning) << "Anycubic LAN: handshake failed: " << result.detail;
        msg = error_text(result.error, result.detail, GUI::from_u8(m_host));
        return false;
    }
    msg = get_test_ok_msg();
    return true;
}

wxString AnycubicLanHost::get_test_ok_msg() const { return _L("Connection to the Anycubic printer is working correctly."); }

wxString AnycubicLanHost::get_test_failed_msg(wxString& msg) const
{
    // TRN %1% is the reason, a sentence or two.
    return GUI::format_wxstr(_L("Could not connect to the Anycubic printer: %1%"), msg);
}

bool AnycubicLanHost::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn) const
{
    const wxString                 host_text = GUI::from_u8(m_host);
    const boost::filesystem::path& source    = upload_data.source_path;
    boost::system::error_code      size_error;
    const std::uintmax_t           size = boost::filesystem::file_size(source, size_error);
    std::string                    head, tail;
    if (size_error || !read_ends(source, size, head, tail)) {
        error_fn(_L("The G-code file to send could not be read."));
        return false;
    }

    // The filaments are matched to the ACE before anything is sent, so a print that cannot start
    // leaves nothing behind on the printer.
    const bool  start = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    std::string ams_settings;
    if (start) {
        const FileFilaments file = file_filaments(head, tail);
        const Result        ace  = read_ace(m_host, http_fetch);
        if (ace.error == Error::NoAce) {
            if (file.used.size() > 1) {
                error_fn(_L("This print uses more than one filament, and the printer reports no ACE unit to feed them."));
                return false;
            }
            ams_settings = no_ace_settings();
        } else if (ace.error != Error::None) {
            BOOST_LOG_TRIVIAL(warning) << "Anycubic LAN: reading the ACE failed: " << ace.detail;
            error_fn(error_text(ace.error, ace.detail, host_text));
            return false;
        } else {
            const FilamentMap map = map_filaments(file, ace.slots);
            if (map.missing >= 0) {
                const std::string material = map.missing < int(file.materials.size()) ? file.materials[map.missing] : std::string();
                // TRN %1% is the filament's number in the project, from 1; %2% its material, such as PLA.
                error_fn(GUI::format_wxstr(_L("Filament %1% (%2%) has no loaded spool of that material in the ACE unit. Load one, or choose another filament, then send the print again."),
                                      map.missing + 1, GUI::from_u8(material)));
                return false;
            }
            ams_settings = map.settings;
        }
    }

    std::string url;
    if (const Error error = upload_address(m_host, http_fetch, url); error != Error::None) {
        error_fn(error_text(error, {}, host_text));
        return false;
    }
    const std::string name = printer_file_name(upload_data.upload_path.string());
    std::string       stored;
    bool              sent = false;
    Http::post(url)
        .timeout_connect(5)
        // The printer checks its free space against this before it takes the file.
        .header("X-File-Length", std::to_string(size))
        .form_add("filename", name)
        .form_add_file("gcode", source, name)
        .on_complete([&](std::string body, unsigned) {
            int code = 0;
            sent     = read_upload_answer(body, name, stored, code);
            if (sent)
                return;
            BOOST_LOG_TRIVIAL(warning) << "Anycubic LAN: upload refused, code " << code;
            if (code == 19010)
                error_fn(_L("The printer does not have enough free space for this file."));
            else
                // TRN %1% is the printer's error number.
                error_fn(GUI::format_wxstr(_L("The printer did not take the file (error %1%)."), code));
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "Anycubic LAN: upload failed: " << error << ", HTTP " << status;
            error_fn(format_error(body, error, status));
        })
        .on_progress([&](Http::Progress progress, bool& cancel) { progress_fn(std::move(progress), cancel); })
        .perform_sync();
    if (!sent || !start)
        return sent;

    std::string md5;
    std::string path = source.string();
    bbl_calc_md5(path, md5);
    std::transform(md5.begin(), md5.end(), md5.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const PrintJob job{stored, static_cast<long long>(size), md5, ams_settings};
    std::string    answer;
    const Result   started = start_print(m_host, http_fetch, job, answer);
    if (started.error == Error::None)
        return true;
    if (started.error == Error::Refused && !answer.empty())
        // TRN %1% is the printer's own message, in English.
        error_fn(GUI::format_wxstr(_L("The file was sent, and the printer did not start it: %1%"), GUI::from_u8(answer)));
    else
        error_fn(error_text(started.error, started.detail, host_text));
    return false;
}

} // namespace Slic3r
