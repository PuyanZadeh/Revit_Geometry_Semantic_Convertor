#include "qcexpansion.h"
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
const char* plugin_name() { return "QC Expansion Plugin"; }

const char* handle_ifc_qcexpansion(const std::string& input_json) {
    static std::string response;
    response = json({{"plugin","QCExpansion"},{"status","stub"}}).dump();
    return response.c_str();
}
}
