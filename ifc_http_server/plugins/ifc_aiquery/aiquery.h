// aiquery.h
#pragma once
#include <string>
#include <nlohmann/json.hpp>

extern "C" {
    const char* plugin_name();
    const char* handle_ifc_aiquery(const std::string& input_json);

    // OBSOLETE (helpers must NOT be declared in extern "C" / header)
    // static bool load_json_mmap(const std::string& path, nlohmann::json& out);
    // static nlohmann::json parse_raw_attrs(const std::string& raw);
    // static nlohmann::json resolve_step_ref(const nlohmann::json& sem_elements,const std::string& step_ref);
}
