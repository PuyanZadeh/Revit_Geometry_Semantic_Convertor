#include "spaceprogramming.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() { return "Space Programming Plugin"; }

const char* handle_ifc_spaceprogramming(const std::string& input_json) {
    static std::string response;
    response = json({{"plugin","SpaceProgramming"},{"status","stub"}}).dump();
    return response.c_str();
}
}
