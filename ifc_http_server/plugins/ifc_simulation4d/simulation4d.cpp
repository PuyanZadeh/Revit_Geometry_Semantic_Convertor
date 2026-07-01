#include "simulation4d.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() { return "4D Simulation Plugin"; }

const char* handle_ifc_simulation4d(const std::string& input_json) {
    static std::string response;
    response = json({{"plugin","Simulation4D"},{"status","stub"}}).dump();
    return response.c_str();
}
}
