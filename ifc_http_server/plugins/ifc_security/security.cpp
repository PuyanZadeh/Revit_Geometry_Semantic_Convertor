#include "security.h"
#include <string>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <ctime>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

extern "C" {

static const std::string SECRET_KEY = "CHANGE_THIS_SECRET_KEY";

const char* plugin_name() {
    return "Security & Access Plugin";
}

std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char* digest;
    digest = HMAC(EVP_sha256(), key.c_str(), key.length(),
                  (unsigned char*)data.c_str(), data.length(), NULL, NULL);

    std::ostringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return ss.str();
}

const char* handle_ifc_security(const std::string& input_json) {
    static std::string response;
    json req;
    try {
        req = json::parse(input_json);
        std::string user = req.value("user", "");
        std::string role = req.value("role", "viewer");
        std::time_t now = std::time(nullptr);
        std::string payload = user + "|" + role + "|" + std::to_string(now);
        std::string token = hmac_sha256(SECRET_KEY, payload);

        json res;
        res["status"] = "ok";
        res["token"] = token;
        res["user"] = user;
        res["role"] = role;
        res["issued"] = now;
        response = res.dump();
    } catch (...) {
        response = R"({"status":"error","message":"invalid input"})";
    }
    return response.c_str();
}

}
