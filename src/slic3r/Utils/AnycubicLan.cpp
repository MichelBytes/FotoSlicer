#include "AnycubicLan.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace Slic3r {
namespace AnycubicLan {

namespace {

using nlohmann::json;
using boost::asio::ip::tcp;

constexpr int         INFO_PORT     = 18910;
constexpr const char* TOPIC_PREFIX  = "anycubic/anycubicCloud/v1";
constexpr std::size_t MAX_PACKET    = 1 << 20; // an ACE report is a few kilobytes
constexpr int         SLOT_LOADED   = 5;

std::string text_of(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The model id is a number, sent as a number or as text.
std::string id_of(const json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end())
        return {};
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    return it->is_string() ? it->get<std::string>() : std::string();
}

std::string random_text(std::size_t length, const std::string& alphabet)
{
    std::random_device                         seed;
    std::mt19937                               engine(seed());
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string                                out;
    for (std::size_t i = 0; i < length; ++i)
        out += alphabet[pick(engine)];
    return out;
}

std::string md5_hex(const std::string& text)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    EVP_Digest(text.data(), text.size(), digest, &length, EVP_md5(), nullptr);
    static const char hex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned int i = 0; i < length; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 15];
    }
    return out;
}

bool base64_decode(std::string text, std::string& out)
{
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }), text.end());
    if (text.empty() || text.size() % 4 != 0)
        return false;
    out.assign(text.size() / 4 * 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]), reinterpret_cast<const unsigned char*>(text.data()),
                                       int(text.size()));
    if (length < 0)
        return false;
    // EVP_DecodeBlock counts the padding as decoded zero bytes.
    const std::size_t padding = text.back() != '=' ? 0 : text[text.size() - 2] == '=' ? 2 : 1;
    out.resize(std::size_t(length) - padding);
    return true;
}

// Where the signed request goes: the path and port the printer names, on the host the user
// entered. ctrlInfoUrl is whatever the device at that address sent; taking its host as well
// would let any device answering on port 18910 send FotoSlicer to a host of its choosing.
bool ctrl_url(const std::string& host, const std::string& supplied, const std::string& query, std::string& url)
{
    static const std::regex shape(R"(^[A-Za-z][A-Za-z0-9+.-]*://[^/:?#]*(?::(\d{1,5}))?([^?#]*)(?:\?([^#]*))?)");
    std::smatch             parts;
    if (!std::regex_search(supplied, parts, shape))
        return false;
    const std::string port = parts[1].matched ? parts[1].str() : std::to_string(INFO_PORT);
    std::string       path = parts[2].str();
    if (path.empty() || path.front() != '/')
        path.insert(path.begin(), '/');
    const std::string own_query = parts[3].str();
    url = "http://" + host + ":" + port + path + "?" + (own_query.empty() ? query : own_query + "&" + query);
    return true;
}

bool read_boxes(const json& data, std::vector<AceSlot>& slots)
{
    const auto boxes = data.find("multi_color_box");
    if (boxes == data.end() || !boxes->is_array())
        return false;
    bool found = false;
    for (const json& box : *boxes) {
        // A finished feed sends an entry that is not a unit; only entries with slots are.
        if (!box.is_object() || !box.contains("id") || !box["id"].is_number_integer() || !box.contains("slots") || !box["slots"].is_array())
            continue;
        found = true;
        for (const json& entry : box["slots"]) {
            if (!entry.is_object() || !entry.contains("index") || !entry["index"].is_number_integer())
                continue;
            AceSlot slot;
            slot.box      = box["id"].get<int>();
            slot.index    = entry["index"].get<int>();
            slot.material = text_of(entry, "type");
            slot.loaded   = entry.contains("status") && entry["status"].is_number_integer() && entry["status"].get<int>() == SLOT_LOADED;
            const auto color = entry.find("color");
            if (color != entry.end() && color->is_array() && color->size() >= 3) {
                static const char hex[] = "0123456789ABCDEF";
                std::string       text  = "#";
                for (std::size_t i = 0; i < 3; ++i) {
                    const int channel = (*color)[i].is_number_integer() ? std::clamp((*color)[i].get<int>(), 0, 255) : 0;
                    text += hex[channel >> 4];
                    text += hex[channel & 15];
                }
                slot.color = text;
            }
            // The unit knows how much is left only from an Anycubic spool's tag; a spool with
            // no tag (no SKU) reads 0, which means "unknown", not "empty".
            const auto percent = entry.find("consumables_percent");
            if (percent != entry.end() && percent->is_number()) {
                const int value = std::clamp(int(percent->get<double>() + 0.5), 0, 100);
                if (value > 0 || !text_of(entry, "sku").empty())
                    slot.percent_left = value;
            }
            slots.push_back(std::move(slot));
        }
    }
    std::sort(slots.begin(), slots.end(), [](const AceSlot& a, const AceSlot& b) { return std::make_pair(a.box, a.index) < std::make_pair(b.box, b.index); });
    return found;
}

// MQTT 3.1.1, the few packets this needs.
void put_u16(std::string& out, std::size_t value)
{
    out += char((value >> 8) & 0xFF);
    out += char(value & 0xFF);
}

void put_text(std::string& out, const std::string& value)
{
    put_u16(out, value.size());
    out += value;
}

std::string packet(std::uint8_t header, const std::string& body)
{
    std::string out(1, char(header));
    std::size_t length = body.size();
    do {
        std::uint8_t byte = length % 128;
        length /= 128;
        if (length > 0)
            byte |= 0x80;
        out += char(byte);
    } while (length > 0);
    return out + body;
}

boost::asio::ssl::context make_tls_context()
{
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
    // The printer's broker presents a self-signed certificate that nothing can be checked
    // against; the connection is encrypted, not authenticated, as in Anycubic's own apps.
    context.set_verify_mode(boost::asio::ssl::verify_none);
    return context;
}

// A TLS connection whose every step ends by one deadline.
class BrokerLink
{
public:
    explicit BrokerLink(std::chrono::steady_clock::time_point deadline) : m_tls(make_tls_context()), m_stream(m_io, m_tls), m_deadline(deadline) {}

    ~BrokerLink()
    {
        boost::system::error_code ignored;
        m_stream.lowest_layer().close(ignored);
    }

    bool connect(const std::string& host, int port)
    {
        boost::system::error_code  error;
        std::vector<tcp::endpoint> endpoints;
        const auto                 address = boost::asio::ip::make_address(host, error);
        if (!error) {
            endpoints.emplace_back(address, static_cast<unsigned short>(port));
        } else {
            tcp::resolver resolver(m_io);
            for (const auto& entry : resolver.resolve(host, std::to_string(port), error))
                endpoints.push_back(entry.endpoint());
            if (error || endpoints.empty())
                return false;
        }
        return run([&](auto done) { boost::asio::async_connect(m_stream.lowest_layer(), endpoints, done); }) &&
               run([&](auto done) { m_stream.async_handshake(boost::asio::ssl::stream_base::client, done); });
    }

    bool write(const std::string& bytes)
    {
        return run([&](auto done) { boost::asio::async_write(m_stream, boost::asio::buffer(bytes), done); });
    }

    bool read_packet(std::uint8_t& header, std::string& body)
    {
        std::string byte;
        if (!read_exact(1, byte))
            return false;
        header             = std::uint8_t(byte[0]);
        std::size_t length = 0;
        for (int shift = 0;; shift += 7) {
            if (shift > 21 || !read_exact(1, byte))
                return false;
            const auto value = std::uint8_t(byte[0]);
            length |= std::size_t(value & 0x7F) << shift;
            if ((value & 0x80) == 0)
                break;
        }
        return length <= MAX_PACKET && read_exact(length, body);
    }

private:
    bool read_exact(std::size_t length, std::string& out)
    {
        out.assign(length, '\0');
        return length == 0 || run([&](auto done) { boost::asio::async_read(m_stream, boost::asio::buffer(&out[0], length), done); });
    }

    // Starts one asynchronous step and runs it until it ends or the deadline passes. On the
    // deadline the socket is closed and the aborted step is run to its end, so its handler
    // never outlives the locals it writes to.
    template<class Start> bool run(Start&& start)
    {
        bool                      finished = false;
        boost::system::error_code result;
        start([&finished, &result](const boost::system::error_code& error, auto&&...) {
            finished = true;
            result   = error;
        });
        m_io.restart();
        m_io.run_until(m_deadline);
        if (!finished) {
            boost::system::error_code ignored;
            m_stream.lowest_layer().close(ignored);
            m_io.restart();
            m_io.run();
            return false;
        }
        return !result;
    }

    boost::asio::io_context                        m_io;
    boost::asio::ssl::context                      m_tls;
    boost::asio::ssl::stream<tcp::socket>          m_stream;
    std::chrono::steady_clock::time_point          m_deadline;
};

// Connects to the printer's broker with the session's credentials and subscribes to the
// printer's reports. False, with `result.error` set, when that fails.
bool open_broker(BrokerLink& link, const std::string& host, Result& result)
{
    const Session& session = result.session;
    // The broker runs on the printer, so it is reached at the address the user entered.
    if (!link.connect(host, session.broker_port)) {
        result.error  = Error::Unreachable;
        result.detail = "broker";
        return false;
    }
    std::string connect;
    put_text(connect, "MQTT");
    connect += char(4);    // protocol level 3.1.1
    connect += char(0xC2); // user name, password, clean session
    put_u16(connect, 60);  // keep-alive, seconds
    put_text(connect, "fotoslicer-" + random_text(8, "0123456789abcdef"));
    put_text(connect, session.username);
    put_text(connect, session.password);
    std::uint8_t header = 0;
    std::string  body;
    if (!link.write(packet(0x10, connect)) || !link.read_packet(header, body)) {
        result.error = Error::NoAnswer;
        return false;
    }
    if ((header & 0xF0) != 0x20 || body.size() < 2 || body[1] != 0) {
        result.error = Error::BrokerRefused;
        return false;
    }
    std::string subscribe;
    put_u16(subscribe, 1);
    put_text(subscribe, std::string(TOPIC_PREFIX) + "/printer/public/" + session.model_id + "/" + session.device_id + "/#");
    subscribe += char(0);
    if (!link.write(packet(0x82, subscribe))) {
        result.error = Error::NoAnswer;
        return false;
    }
    // Wait for the subscription, so the answer to what is sent next cannot be missed.
    while (link.read_packet(header, body)) {
        if ((header & 0xF0) != 0x90)
            continue;
        if (body.size() < 3 || std::uint8_t(body[2]) == 0x80) {
            result.error = Error::BrokerRefused;
            return false;
        }
        return true;
    }
    result.error = Error::NoAnswer;
    return false;
}

// Sends one PUBLISH, QoS 0, to the printer's command topic for `type`.
bool publish(BrokerLink& link, const Session& session, const std::string& type, const std::string& payload)
{
    std::string publish;
    put_text(publish, std::string(TOPIC_PREFIX) + "/web/printer/" + session.model_id + "/" + session.device_id + "/" + type);
    publish += payload;
    return link.write(packet(0x30, publish));
}

// Reads the next PUBLISH: its topic and payload. Acknowledges QoS 1. False when the link ends.
bool next_report(BrokerLink& link, std::string& topic, std::string& payload)
{
    std::uint8_t header = 0;
    std::string  body;
    while (link.read_packet(header, body)) {
        if ((header & 0xF0) != 0x30 || body.size() < 2)
            continue;
        const std::size_t topic_length = (std::size_t(std::uint8_t(body[0])) << 8) | std::uint8_t(body[1]);
        const int         qos          = (header >> 1) & 3;
        const std::size_t offset       = 2 + topic_length + (qos > 0 ? 2 : 0);
        if (offset > body.size())
            continue;
        if (qos == 1)
            link.write(packet(0x40, body.substr(2 + topic_length, 2)));
        topic   = body.substr(2, topic_length);
        payload = body.substr(offset);
        return true;
    }
    return false;
}

long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

Result failed(Result result, Error error, std::string detail = {})
{
    result.error  = error;
    result.detail = std::move(detail);
    return result;
}

} // namespace

std::string sign(const std::string& token, long long ts, const std::string& nonce)
{
    return md5_hex(md5_hex(token.substr(0, 16)) + std::to_string(ts) + nonce);
}

bool decrypt_ctrl(const std::string& info_base64, const std::string& token, const std::string& local_token, std::string& plaintext)
{
    std::string sealed;
    if (token.size() < 32 || !base64_decode(info_base64, sealed) || sealed.empty() || sealed.size() % 16 != 0)
        return false;
    unsigned char iv[16] = {};
    std::copy_n(local_token.begin(), std::min<std::size_t>(16, local_token.size()), iv);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr)
        return false;
    std::string out(sealed.size() + 16, '\0');
    int         first = 0, last = 0;
    const bool  ok = EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char*>(token.data() + 16), iv) == 1 &&
                    EVP_DecryptUpdate(context, reinterpret_cast<unsigned char*>(&out[0]), &first,
                                      reinterpret_cast<const unsigned char*>(sealed.data()), int(sealed.size())) == 1 &&
                    EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char*>(&out[first]), &last) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok)
        return false;
    out.resize(std::size_t(first + last));
    plaintext = std::move(out);
    return true;
}

bool parse_multicolorbox(const std::string& data_json, std::vector<AceSlot>& slots)
{
    const json data = json::parse(data_json, nullptr, false);
    return data.is_object() && read_boxes(data, slots);
}

Result handshake(const std::string& host, const Fetch& fetch)
{
    Result      result;
    std::string body, error;
    if (!fetch("GET", "http://" + host + ":" + std::to_string(INFO_PORT) + "/info", body, error))
        return failed(result, Error::Unreachable, error);
    const json info = json::parse(body, nullptr, false);
    if (!info.is_object())
        return failed(result, Error::NotAPrinter);
    result.session.model_name = text_of(info, "modelName");
    if (text_of(info, "ctrlType") == "cloud")
        return failed(result, Error::CloudMode);
    const std::string token    = text_of(info, "token");
    const std::string supplied = text_of(info, "ctrlInfoUrl");
    result.session.model_id    = id_of(info, "modelId");
    if (token.size() < 32 || supplied.empty() || result.session.model_id.empty())
        return failed(result, Error::NotOffered);

    const long long   ts    = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string nonce = random_text(6, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    const std::string did   = random_text(32, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    std::string       url;
    if (!ctrl_url(host, supplied, "ts=" + std::to_string(ts) + "&nonce=" + nonce + "&sign=" + sign(token, ts, nonce) + "&did=" + did, url))
        return failed(result, Error::NotAPrinter);
    if (!fetch("POST", url, body, error))
        return failed(result, Error::Unreachable, error);
    const json ctrl = json::parse(body, nullptr, false);
    if (!ctrl.is_object())
        return failed(result, Error::NotAPrinter);
    if (!ctrl.contains("code") || !ctrl["code"].is_number_integer() || ctrl["code"].get<long long>() != 200)
        return failed(result, Error::Refused);
    const auto sealed = ctrl.find("data");
    if (sealed == ctrl.end() || !sealed->is_object() || text_of(*sealed, "info").empty() || text_of(*sealed, "token").empty())
        return failed(result, Error::NotAPrinter);
    std::string plaintext;
    if (!decrypt_ctrl(text_of(*sealed, "info"), token, text_of(*sealed, "token"), plaintext))
        return failed(result, Error::Unreadable);
    const json  data = json::parse(plaintext, nullptr, false);
    std::smatch broker;
    const std::string broker_url = data.is_object() ? text_of(data, "broker") : std::string();
    static const std::regex broker_shape(R"(^mqtts?://[^:/]+:(\d{1,5}))");
    if (!std::regex_search(broker_url, broker, broker_shape) || !data.contains("username") || !data.contains("password"))
        return failed(result, Error::Unreadable);
    result.session.broker_port = std::stoi(broker[1].str());
    result.session.username    = text_of(data, "username");
    result.session.password    = text_of(data, "password");
    result.session.device_id   = id_of(data, "deviceId");
    if (result.session.device_id.empty())
        return failed(result, Error::Unreadable);
    return result;
}

Result read_ace(const std::string& host, const Fetch& fetch, int timeout_ms)
{
    Result result = handshake(host, fetch);
    if (result.error != Error::None)
        return result;
    BrokerLink link(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
    if (!open_broker(link, host, result))
        return result;
    const json query = {{"type", "multiColorBox"}, {"action", "getInfo"}, {"timestamp", now_ms()},
                        {"msgid", random_text(32, "0123456789abcdef")}, {"data", nullptr}};
    if (!publish(link, result.session, "multiColorBox", query.dump()))
        return failed(result, Error::NoAnswer);
    std::string topic, payload;
    while (next_report(link, topic, payload)) {
        const json report = json::parse(payload, nullptr, false);
        if (!report.is_object())
            continue;
        std::string type = text_of(report, "type");
        if (type.empty())
            type = topic.substr(topic.rfind('/') + 1);
        const auto data = report.find("data");
        if (type != "multiColorBox" || data == report.end() || !data->is_object())
            continue;
        if (read_boxes(*data, result.slots)) {
            link.write(packet(0xE0, {}));
            return result;
        }
        const auto listed = data->find("multi_color_box");
        if (listed != data->end() && listed->is_array() && listed->empty())
            return failed(result, Error::NoAce);
    }
    return failed(result, Error::NoAnswer);
}

Error upload_address(const std::string& host, const Fetch& fetch, std::string& url)
{
    std::string body, error;
    if (!fetch("GET", "http://" + host + ":" + std::to_string(INFO_PORT) + "/info", body, error))
        return Error::Unreachable;
    const json info = json::parse(body, nullptr, false);
    if (!info.is_object())
        return Error::NotAPrinter;
    if (text_of(info, "ctrlType") == "cloud")
        return Error::CloudMode;
    const std::string supplied = text_of(info, "fileUploadurl");
    if (supplied.empty())
        return Error::NotOffered;
    // The path and query the printer names, on the host the user entered, as for /ctrl.
    std::string path_url;
    if (!ctrl_url(host, supplied, "", path_url))
        return Error::NotAPrinter;
    // ctrl_url always appends "?" and the extra query: drop what an empty one left behind.
    while (!path_url.empty() && (path_url.back() == '?' || path_url.back() == '&'))
        path_url.pop_back();
    url = path_url;
    return Error::None;
}

Result start_print(const std::string& host, const Fetch& fetch, const PrintJob& job, std::string& answer, int timeout_ms)
{
    Result result = handshake(host, fetch);
    if (result.error != Error::None)
        return result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    BrokerLink link(deadline);
    if (!open_broker(link, host, result))
        return result;
    const json ams = json::parse(job.ams_settings, nullptr, false);
    const std::string taskid = std::to_string(100000000 + std::random_device{}() % 900000000);
    // Leveling, vibration compensation, flow calibration, timelapse, camera detection and
    // drying stay off, as the reference client sends: a start with leveling on can hang on the
    // printer waiting for its bed to preheat.
    const json start = {{"type", "print"},
                        {"action", "start"},
                        {"timestamp", now_ms()},
                        {"msgid", random_text(32, "0123456789abcdef")},
                        {"data",
                         {{"taskid", taskid},
                          {"filename", job.file_name},
                          {"filepath", "/"},
                          {"filetype", 1},
                          {"filesize", job.file_size},
                          {"md5", job.md5},
                          {"ams_settings", ams.is_object() ? ams : json{{"use_ams", false}, {"ams_box_mapping", json::array()}}},
                          {"task_settings",
                           {{"auto_leveling", 0},
                            {"vibration_compensation", 0},
                            {"flow_calibration", 0},
                            {"timelapse", {{"status", 0}}},
                            {"ai_settings", {{"status", 0}}},
                            {"dry_mode", 0}}}}}};
    if (!publish(link, result.session, "print", start.dump()))
        return failed(result, Error::NoAnswer);
    // The printer reports on "print" when it takes or refuses the start; no report in time is
    // taken as taken, since a busy printer can be slow to say so.
    std::string topic, payload;
    while (next_report(link, topic, payload)) {
        const json report = json::parse(payload, nullptr, false);
        if (!report.is_object() || text_of(report, "type") != "print")
            continue;
        const auto code = report.find("code");
        if (code != report.end() && code->is_number_integer() && code->get<int>() != 200) {
            answer = text_of(report, "msg");
            if (answer.empty())
                answer = "code " + std::to_string(code->get<int>());
            return failed(result, Error::Refused);
        }
        if (text_of(report, "action") == "start")
            break;
    }
    link.write(packet(0xE0, {}));
    return result;
}

namespace {

// The "; key = value" comments in `text`, keys in lower case with single spaces. The first wins.
void read_settings(const std::string& text, bool drop_first, bool drop_last, std::map<std::string, std::string>& found)
{
    static const std::regex setting(R"(^;\s*([A-Za-z_][A-Za-z0-9_ \[\]]*?)\s*=\s*(.*?)\s*$)");
    std::size_t start = 0;
    if (drop_first) {
        start = text.find('\n');
        start = start == std::string::npos ? text.size() : start + 1;
    }
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            if (drop_last)
                break;
            end = text.size();
        }
        // Settings lines are short; a long one is G-code or a custom G-code block.
        if (text[start] == ';' && end - start < 4096) {
            std::smatch match;
            const std::string line = text.substr(start, end - start);
            if (std::regex_match(line, match, setting)) {
                std::string key;
                for (char c : match[1].str()) {
                    if (c == ' ' && (key.empty() || key.back() == ' '))
                        continue;
                    key += char(std::tolower(static_cast<unsigned char>(c)));
                }
                while (!key.empty() && key.back() == ' ')
                    key.pop_back();
                found.emplace(key, match[2].str());
            }
        }
        start = end + 1;
    }
}

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::size_t              start = 0;
    while (true) {
        const std::size_t end  = text.find(separator, start);
        std::string       part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto        first = part.find_first_not_of(" \t\"");
        const auto        last  = part.find_last_not_of(" \t\"\r");
        parts.push_back(first == std::string::npos ? std::string() : part.substr(first, last - first + 1));
        if (end == std::string::npos)
            return parts;
        start = end + 1;
    }
}

// "#RRGGBB" in upper case, or empty.
std::string normal_color(const std::string& text)
{
    if (text.size() < 7 || text[0] != '#')
        return {};
    std::string color = "#";
    for (std::size_t i = 1; i < 7; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i])))
            return {};
        color += char(std::toupper(static_cast<unsigned char>(text[i])));
    }
    return color;
}

json rgba(const std::string& color)
{
    const std::string normal = normal_color(color);
    if (normal.empty())
        return json::array({255, 255, 255, 255});
    json channels = json::array();
    for (std::size_t i = 1; i < 7; i += 2)
        channels.push_back(std::stoi(normal.substr(i, 2), nullptr, 16));
    channels.push_back(255);
    return channels;
}

int color_distance(const std::string& a, const std::string& b)
{
    const json x = rgba(a), y = rgba(b);
    int        sum = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const int d = x[i].get<int>() - y[i].get<int>();
        sum += d * d;
    }
    return sum;
}

// A material's family: its first word in upper case. "PLA+", "PLA Silk" and "PLA-CF" are PLA.
std::string family(const std::string& material)
{
    std::string word;
    for (char c : material) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            word += char(std::toupper(static_cast<unsigned char>(c)));
        else if (!word.empty())
            break;
    }
    return word;
}

} // namespace

FileFilaments file_filaments(const std::string& head, const std::string& tail)
{
    std::map<std::string, std::string> settings;
    read_settings(head, false, !tail.empty(), settings);
    if (!tail.empty())
        read_settings(tail, true, false, settings);

    FileFilaments file;
    if (const auto types = settings.find("filament_type"); types != settings.end())
        file.materials = split(types->second, ';');
    if (const auto colors = settings.find("filament_colour"); colors != settings.end())
        for (const std::string& color : split(colors->second, ';'))
            file.colors.push_back(normal_color(color));
    // The amount each filament used, by weight or length: a filament with more than none is used.
    // A line with anything unreadable in it says nothing.
    for (const char* key : {"filament used [g]", "filament used [mm]"}) {
        const auto amounts = settings.find(key);
        if (amounts == settings.end())
            continue;
        std::vector<int> used;
        bool             readable = true;
        const auto       parts    = split(amounts->second, ',');
        for (std::size_t i = 0; i < parts.size() && readable; ++i) {
            char*        end   = nullptr;
            const double value = std::strtod(parts[i].c_str(), &end);
            readable           = !parts[i].empty() && end == parts[i].c_str() + parts[i].size() && std::isfinite(value);
            if (readable && value > 0)
                used.push_back(int(i));
        }
        if (readable && !used.empty()) {
            file.used = std::move(used);
            break;
        }
    }
    // A file that does not say what it uses prints with the first filament it names.
    if (file.used.empty())
        file.used.push_back(0);
    return file;
}

FilamentMap map_filaments(const FileFilaments& file, const std::vector<AceSlot>& slots)
{
    FilamentMap map;
    if (slots.empty()) {
        map.missing = file.used.empty() ? 0 : file.used.front();
        return map;
    }
    const int primary = std::min_element(slots.begin(), slots.end(), [](const AceSlot& a, const AceSlot& b) { return a.box < b.box; })->box;
    std::vector<const AceSlot*> spools;
    for (const AceSlot& slot : slots)
        if (slot.box == primary && slot.loaded)
            spools.push_back(&slot);

    std::vector<const AceSlot*> taken;
    const auto is_taken = [&taken](const AceSlot* slot) { return std::find(taken.begin(), taken.end(), slot) != taken.end(); };
    json       mapping  = json::array();
    for (int filament : file.used) {
        const std::string material = filament < int(file.materials.size()) ? file.materials[filament] : std::string();
        const std::string color    = filament < int(file.colors.size()) ? file.colors[filament] : std::string();
        const std::string wanted   = family(material);
        // A slot whose material is unknown fits any filament; so does any slot for a filament
        // whose material is unknown.
        const auto fits = [&wanted](const AceSlot* slot) {
            const std::string loaded = family(slot->material);
            return wanted.empty() || loaded.empty() || loaded == wanted;
        };
        // The same colour; then the slot of the same number, as slicers map by default; then the
        // nearest colour. A slot no other filament took first.
        const auto pick = [&](bool shared) -> const AceSlot* {
            std::vector<const AceSlot*> free;
            for (const AceSlot* slot : spools)
                if (fits(slot) && (shared || !is_taken(slot)))
                    free.push_back(slot);
            for (const AceSlot* slot : free)
                if (!color.empty() && normal_color(slot->color) == color)
                    return slot;
            for (const AceSlot* slot : free)
                if (slot->index == filament)
                    return slot;
            const auto nearest = std::min_element(free.begin(), free.end(), [&color](const AceSlot* a, const AceSlot* b) {
                return color_distance(color, a->color) < color_distance(color, b->color);
            });
            return nearest == free.end() ? nullptr : *nearest;
        };
        const AceSlot* chosen = pick(false);
        if (chosen == nullptr)
            chosen = pick(true);
        if (chosen == nullptr) {
            map.missing = filament;
            return map;
        }
        taken.push_back(chosen);
        mapping.push_back({{"paint_index", filament},
                           {"ams_index", chosen->index},
                           {"paint_color", rgba(color.empty() ? chosen->color : color)},
                           {"ams_color", rgba(chosen->color)},
                           {"material_type", material.empty() ? chosen->material : material}});
    }
    map.settings = json{{"use_ams", true}, {"ams_box_mapping", mapping}}.dump();
    return map;
}

std::string no_ace_settings() { return json{{"use_ams", false}, {"ams_box_mapping", json::array()}}.dump(); }

std::string printer_file_name(const std::string& name)
{
    std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    const std::string suffix = ".gcode";
    if (base.size() >= suffix.size() &&
        std::equal(suffix.rbegin(), suffix.rend(), base.rbegin(), [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
        base.erase(base.size() - suffix.size());
    std::string stem;
    for (std::size_t i = 0; i < base.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(base[i]);
        if (c >= 0x80) {
            // One "_" for each UTF-8 character, not for each of its bytes.
            if ((c & 0xC0) != 0x80)
                stem += '_';
            continue;
        }
        stem += std::isalnum(c) || std::strchr(" ._()+-", c) != nullptr ? char(c) : '_';
    }
    const std::size_t first = stem.find_first_not_of(". ");
    stem                    = first == std::string::npos ? std::string() : stem.substr(first);
    stem                    = stem.substr(0, 100 - suffix.size());
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    return (stem.empty() ? std::string("print") : stem) + suffix;
}

bool read_upload_answer(const std::string& body, const std::string& sent, std::string& stored, int& code)
{
    code              = 0;
    const json answer = json::parse(body, nullptr, false);
    if (!answer.is_object())
        return false;
    // A good upload answers with code 0 or 200, or none.
    const auto given = answer.find("code");
    if (given != answer.end()) {
        if (!given->is_number_integer())
            return false;
        code = given->get<int>();
        if (code != 0 && code != 200)
            return false;
    }
    stored            = sent;
    const auto data   = answer.find("data");
    if (data != answer.end() && data->is_object()) {
        const std::string kept = text_of(*data, "gcode");
        static const std::regex plain(R"([A-Za-z0-9 ._()+\-]{1,255})");
        if (std::regex_match(kept, plain) && kept.front() != '.' && kept.size() > 6 &&
            kept.compare(kept.size() - 6, 6, ".gcode") == 0)
            stored = kept;
    }
    code = 0;
    return true;
}

} // namespace AnycubicLan
} // namespace Slic3r
