#include "logging.h"
#include <fstream>
#include <ctime>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;
static const std::string LOG_PATH = "/home/puyan_zadeh/ifc_http_server/logs/ifc_http_server_plugin.log";

extern "C" {

const char* plugin_name() {
    return "Logging & Metrics Plugin";
}

const char* handle_ifc_logging(const std::string& input_json) {
    static std::string response;
    try {
        json req = json::parse(input_json);
        std::string action = req.value("action", "");
        std::string message = req.value("message", "");

        if (action == "get") {
            std::vector<std::string> lines;
            std::ifstream f(LOG_PATH);
            std::string line;
            while (std::getline(f, line)) lines.push_back(line);
            response = json({{"plugin","Logging"},{"logs",lines}}).dump();
            return response.c_str();
        }

        if (action == "clear") {
            std::ofstream f(LOG_PATH, std::ios::trunc);
            f.close();
            response = R"({"plugin":"Logging","status":"cleared"})";
            return response.c_str();
        }

        // Default: append message
        std::ofstream log(LOG_PATH, std::ios::app);
        std::time_t now = std::time(nullptr);
        log << "[" << std::ctime(&now) << "] " << message << std::endl;
        response = R"({"plugin":"Logging","status":"ok"})";
    }
    catch (...) {
        response = R"({"plugin":"Logging","error":"invalid input"})";
    }
    return response.c_str();
}

}
