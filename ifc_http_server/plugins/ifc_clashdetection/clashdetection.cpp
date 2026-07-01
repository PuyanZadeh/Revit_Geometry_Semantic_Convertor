#include "clashdetection.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() { return "Clash Detection Plugin"; }

const char* handle_ifc_clashdetection(const std::string& input_json) {
static std::string response;
    response = json({{"plugin","ClashDetection"},{"status","stub"}}).dump();
    return response.c_str();
}
}
