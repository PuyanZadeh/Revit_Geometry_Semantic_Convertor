#include "federation.h"
#include <string>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <sstream>

using json = nlohmann::json;

extern "C" {

const char* plugin_name() {
    return "Federation Plugin";
}

// Merge IFC models using IfcOpenShell IfcConvert
std::string merge_ifc_models(const std::vector<std::string>& files, const std::string& output_file) {
    std::ostringstream output;
    std::ostringstream command;
    command << "/home/puyan_zadeh/core/ifcopenshell/build/IfcConvert";

    for (const auto& f : files)
        command << " \"" << f << "\"";

    command << " \"" << output_file << "\"";

    std::array<char, 256> buffer{};
    FILE* pipe = popen(command.str().c_str(), "r");
    if (!pipe) return "Error: Unable to run IfcConvert";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        output << buffer.data();

    int result = pclose(pipe);
    if (result == 0)
        return "OK";
    else
        return "Error: " + output.str();
}

const char* handle_ifc_federation(const std::string& input_json) {
    static std::string response;
    try {
        json req = json::parse(input_json);
        if (!req.contains("files") || !req["files"].is_array()) {
            response = R"({"error":"missing or invalid 'files' array"})";
            return response.c_str();
        }

        std::vector<std::string> files = req["files"].get<std::vector<std::string>>();
        std::string output = req.value("output", "/tmp/federated.ifc");

        std::string result = merge_ifc_models(files, output);

        json res;
        res["plugin"] = "Federation";
        res["merged_files"] = files;
        res["output"] = output;
        res["status"] = result;

        response = res.dump();
    } catch (...) {
        response = R"({"error":"invalid input"})";
    }
    return response.c_str();
}

}
