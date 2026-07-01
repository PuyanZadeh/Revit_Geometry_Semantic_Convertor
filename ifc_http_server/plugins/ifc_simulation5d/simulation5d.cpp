#include "simulation5d.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() { return "5D Simulation Plugin"; }

const char* handle_ifc_simulation5d(const std::string& input_json) {
    static std::string response;
    response = json({{"plugin","Simulation5D"},{"status","stub"}}).dump();
    return response.c_str();
}
}
