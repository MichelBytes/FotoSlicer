#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace AnycubicLan {

// Reads what an Anycubic printer in LAN Mode reports about the spools in its ACE units.
//
// The printer answers on the local network only, in two steps: an HTTP handshake on port
// 18910 (GET /info, then a signed POST to the control URL it names) hands out credentials,
// sealed with AES-128-CBC, for an MQTT broker the printer runs over TLS. The ACE state is
// not pushed; it is the answer to a "multiColorBox"/"getInfo" query on that broker.

// One spool position in an ACE unit.
struct AceSlot
{
    int         box{0};           // the ACE unit, from 0
    int         index{0};         // the slot in the unit, from 0
    std::string material;         // "PLA", "PETG", ...; empty when the slot holds nothing
    std::string color;            // "#RRGGBB"; empty when unknown
    int         percent_left{-1}; // 0..100, or -1 when the printer cannot tell (a spool with no tag)
    bool        loaded{false};    // the spool is fed into the unit
};

// The broker session the handshake hands out.
struct Session
{
    int         broker_port{0};
    std::string username;
    std::string password;
    std::string device_id;
    std::string model_id;
    std::string model_name;
};

enum class Error {
    None,
    Unreachable,   // nothing answered at the address
    NotAPrinter,   // something answered, and not the way a printer does
    CloudMode,     // the printer is not in LAN Mode
    NotOffered,    // firmware without this LAN connection
    Refused,       // the printer refused the signed request
    Unreadable,    // the sealed credentials could not be opened
    BrokerRefused, // the printer's broker did not accept the credentials
    NoAnswer,      // the broker never answered the ACE query
    NoAce,         // the printer answered and has no ACE unit attached
};

struct Result
{
    Error                error{Error::None};
    std::string          detail; // transport error text, for the log
    Session              session;
    std::vector<AceSlot> slots;
};

// Makes one HTTP request (method "GET" or "POST", no body) and stores the response body.
// Returns false, with a short reason in `error`, when no answer came back.
using Fetch = std::function<bool(const std::string& method, const std::string& url, std::string& body, std::string& error)>;

// md5(md5(token[0..16]) + ts + nonce), as lowercase hex.
std::string sign(const std::string& token, long long ts, const std::string& nonce);

// Opens the /ctrl answer's sealed "info": AES-128-CBC, key token[16..32], IV the answer's
// own token padded or cut to 16 bytes, PKCS#7 padding.
bool decrypt_ctrl(const std::string& info_base64, const std::string& token, const std::string& local_token, std::string& plaintext);

// Reads the slots of every ACE unit in a multiColorBox report's "data" object (JSON text).
// Returns false when the report lists no unit.
bool parse_multicolorbox(const std::string& data_json, std::vector<AceSlot>& slots);

// The handshake alone: GET /info, signed POST, decrypt. Blocking.
Result handshake(const std::string& host, const Fetch& fetch);

// The handshake, then the ACE query on the printer's broker. Blocking for up to
// `timeout_ms` after the handshake: call it from a worker thread.
Result read_ace(const std::string& host, const Fetch& fetch, int timeout_ms = 15000);

} // namespace AnycubicLan
} // namespace Slic3r
