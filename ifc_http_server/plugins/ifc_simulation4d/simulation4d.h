#pragma once
#include <string>

extern "C" {
    const char* plugin_name();
    const char* handle_ifc_simulation4d(const std::string& input_json);
}
