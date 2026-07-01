#include "qc.h"
#include <string>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <sstream>

using json = nlohmann::json;

extern "C" {

const char* plugin_name() {
    return "Quality Control Plugin";
}

// Runs IfcOpenShell's parser check
std::string run_ifc_check(const std::string& file_path) {
    std::ostringstream output;
    std::string command = "/home/puyan_zadeh/core/ifcopenshell/build/IfcConvert \"" + file_path + "\" /tmp/qc_check.obj";
    std::array<char, 256> buffer{};

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "Error: Unable to run IfcConvert";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        output << buffer.data();

    int result = pclose(pipe);
    if (result == 0)
        return "OK";
    else
        return "Error: " + output.str();
}

const char* handle_ifc_qc(const std::string& input_json) {
    static std::string response;
    try {
        json req = json::parse(input_json);
        std::string file_path = req.value("file", "");

        if (file_path.empty()) {
            response = R"({"error":"missing file path"})";
            return response.c_str();
        }

        json result;
        result["plugin"] = "QualityControl";
        result["check"] = "IFC Conformance";
        result["file"] = file_path;

        std::string ifc_status = run_ifc_check(file_path);
        result["ifc_conformance"] = ifc_status;

        // Placeholders for future submodules
        result["owner_requirements"] = "pending";
        result["code_compliance"] = "pending";

        response = result.dump();
    } catch (...) {
        response = R"({"error":"invalid input"})";
    }

    return response.c_str();
}

}
