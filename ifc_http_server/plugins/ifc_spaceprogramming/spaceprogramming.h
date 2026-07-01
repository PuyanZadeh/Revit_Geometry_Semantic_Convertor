#pragma once
#include <string>

extern "C" {
    const char* plugin_name();
    const char* handle_ifc_spaceprogramming(const std::string& input_json);
}
