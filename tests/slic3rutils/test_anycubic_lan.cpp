#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "slic3r/Utils/AnycubicLan.hpp"

using namespace Slic3r::AnycubicLan;

namespace {
const std::string TOKEN = "0123456789abcdefFEDCBA9876543210";

// What a fake handshake returns: /info, then /ctrl with credentials sealed under TOKEN.
Fetch fake_printer(std::string ctrl_type, std::vector<std::string>* urls)
{
    return [ctrl_type, urls](const std::string& method, const std::string& url, std::string& body, std::string&) {
        urls->push_back(method + " " + url);
        if (method == "GET")
            body = R"({"token":")" + TOKEN + R"(","ctrlInfoUrl":"http://10.9.9.9:18911/ctrl","modelId":20028,"modelName":"Anycubic Kobra 4","ctrlType":")" + ctrl_type + "\"}";
        else
            body = R"({"code":200,"data":{"token":"localtokenABC","info":"Ef3p84wk5a+k7l/yC06MsztfY4R5h8NMFS+xdkvWRoZ/VAnVljUUFCt2XCdBBWEP4RqDlEr3H/WXCgf2dplyRQ4IsV7clXRWvJ8JQGsIyE7wDePItNHn8sysnrmycFat"}})";
        return true;
    };
}
} // namespace

TEST_CASE("Anycubic LAN signature matches the printer's", "[AnycubicLan]")
{
    // md5(md5(token[:16]) + ts + nonce), checked against Python's hashlib.
    CHECK(sign(TOKEN, 1700000000000LL, "aB3dE6") == "1c27191292623fe78b097ff70a0c7d0e");
}

TEST_CASE("Anycubic LAN handshake opens the sealed credentials", "[AnycubicLan]")
{
    std::vector<std::string> urls;
    const Result             result = handshake("192.168.1.20", fake_printer("lan", &urls));
    REQUIRE(result.error == Error::None);
    CHECK(result.session.broker_port == 9883);
    CHECK(result.session.username == "u");
    CHECK(result.session.password == "p");
    CHECK(result.session.device_id == "d");
    CHECK(result.session.model_id == "20028");
    REQUIRE(urls.size() == 2);
    CHECK(urls[0] == "GET http://192.168.1.20:18910/info");
    // The signed request keeps the printer's port and path, but goes to the address entered.
    CHECK(urls[1].rfind("POST http://192.168.1.20:18911/ctrl?ts=", 0) == 0);

    std::string plaintext;
    CHECK_FALSE(decrypt_ctrl("not base64!", TOKEN, "localtokenABC", plaintext));
}

TEST_CASE("Anycubic LAN handshake reports cloud mode", "[AnycubicLan]")
{
    std::vector<std::string> urls;
    CHECK(handshake("192.168.1.20", fake_printer("cloud", &urls)).error == Error::CloudMode);
    CHECK(urls.size() == 1);
}

TEST_CASE("ACE slots are read from a multiColorBox report", "[AnycubicLan]")
{
    std::vector<AceSlot> slots;
    REQUIRE(parse_multicolorbox(R"({"multi_color_box":[{"id":0,"status":1,"model_id":40002,"slots":[
        {"index":1,"sku":"AHPEFG-102","type":"PETG","color":[67,82,59],"status":5,"consumables_percent":100},
        {"index":0,"sku":"","type":"PLA","color":[255,255,255],"status":5,"consumables_percent":0},
        {"index":2,"sku":"","type":"","color":[0,0,0],"status":0,"consumables_percent":0}]}]})", slots));
    REQUIRE(slots.size() == 3);
    CHECK(slots[0].index == 0);
    CHECK(slots[0].material == "PLA");
    CHECK(slots[0].color == "#FFFFFF");
    CHECK(slots[0].percent_left == -1); // no tag: unknown, not empty
    CHECK(slots[0].loaded);
    CHECK(slots[1].color == "#43523B");
    CHECK(slots[1].percent_left == 100);
    CHECK(slots[2].material.empty());
    CHECK_FALSE(slots[2].loaded);

    slots.clear();
    CHECK_FALSE(parse_multicolorbox(R"({"multi_color_box":[]})", slots));
    CHECK_FALSE(parse_multicolorbox("not json", slots));
}
