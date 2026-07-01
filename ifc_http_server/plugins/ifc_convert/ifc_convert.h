#pragma once
#include <string>

extern "C" {
    const char* plugin_name();
    const char* handle_ifc_convert(const std::string& input_json);
    std::string export_ifc_metadata_json(const std::string& ifc_path);
    std::string export_ifc_object_index(const std::string& ifc_path);
    std::string export_ifc_property_data(const std::string& ifc_path);
    std::string export_ifc_geometry_mapping(const std::string& ifc_path);
    std::string export_ifc_semantic_attributes(const std::string& ifc_path);
    std::string export_ifc_full_semantics(const std::string& ifc_path);
    std::string export_ifc_property_sets(const std::string& ifc_path);
    std::string export_ifc_semantic_json(const std::string& ifc_path);
    std::string run_assimp_convert(const std::string& glb_path, std::string& assimp_err);
}
