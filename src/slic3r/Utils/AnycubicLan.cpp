#include "AnycubicLan.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
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
    const Session& session = result.session;

    // The broker runs on the printer, so it is reached at the address the user entered, for
    // the same reason as the signed request.
    BrokerLink link(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
    if (!link.connect(host, session.broker_port))
        return failed(result, Error::Unreachable, "broker");

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
    if (!link.write(packet(0x10, connect)) || !link.read_packet(header, body))
        return failed(result, Error::NoAnswer);
    if ((header & 0xF0) != 0x20 || body.size() < 2 || body[1] != 0)
        return failed(result, Error::BrokerRefused);

    const std::string device = session.model_id + "/" + session.device_id;
    std::string       subscribe;
    put_u16(subscribe, 1);
    put_text(subscribe, std::string(TOPIC_PREFIX) + "/printer/public/" + device + "/#");
    subscribe += char(0);
    if (!link.write(packet(0x82, subscribe)))
        return failed(result, Error::NoAnswer);

    const json query = {{"type", "multiColorBox"},
                        {"action", "getInfo"},
                        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
                        {"msgid", random_text(32, "0123456789abcdef")},
                        {"data", nullptr}};
    std::string publish;
    put_text(publish, std::string(TOPIC_PREFIX) + "/web/printer/" + device + "/multiColorBox");
    publish += query.dump();

    bool asked = false;
    while (link.read_packet(header, body)) {
        const std::uint8_t kind = header & 0xF0;
        if (kind == 0x90 && !asked) { // SUBACK: ask once the reports can arrive
            if (body.size() < 3 || std::uint8_t(body[2]) == 0x80)
                return failed(result, Error::BrokerRefused);
            if (!link.write(packet(0x30, publish)))
                break;
            asked = true;
            continue;
        }
        if (kind != 0x30 || body.size() < 2)
            continue;
        const std::size_t topic_length = (std::size_t(std::uint8_t(body[0])) << 8) | std::uint8_t(body[1]);
        const int         qos          = (header >> 1) & 3;
        std::size_t       offset       = 2 + topic_length + (qos > 0 ? 2 : 0);
        if (offset > body.size())
            continue;
        if (qos == 1) {
            std::string ack = body.substr(2 + topic_length, 2);
            link.write(packet(0x40, ack));
        }
        if (!asked)
            continue;
        const std::string topic  = body.substr(2, topic_length);
        const json        report = json::parse(body.substr(offset), nullptr, false);
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

} // namespace AnycubicLan
} // namespace Slic3r
