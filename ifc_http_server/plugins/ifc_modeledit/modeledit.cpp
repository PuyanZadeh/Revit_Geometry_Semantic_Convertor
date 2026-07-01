#include "modeledit.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() {
    return "Model Edit Plugin";
}

const char* handle_ifc_modeledit(const std::string& input_json) {
    static std::string response;
    response = json({{"plugin","ModelEdit"},{"status","stub"}}).dump();
    return response.c_str();
}
}
