#include "visualization.h"
#include "list_glb.h"
#include <string>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>

#include <filesystem>
namespace fs = std::filesystem;

static fs::path exe_dir() { return fs::canonical("/proc/self/exe").parent_path(); }
static fs::path server_root() { return exe_dir().parent_path(); }
static fs::path data_root() { return server_root().parent_path() / "storage"; }
static std::string gltf_dir() { return (data_root() / "outputs" / "gltf").string(); }

using json = nlohmann::json;

extern "C" {

const char* plugin_name() {
    return "Visualization Plugin";
}

const char* handle_ifc_visualization(const std::string& input_json) {
    static std::string response;
    //thread_local static std::string response;
    json req;

    try {
        req = json::parse(input_json);

        if (req.value("action", "") == "list_glb") {
            //auto files = list_glb_files("/home/puyan_zadeh/storage/outputs/gltf");
            auto files = list_glb_files(gltf_dir());

            json out = { {"files", files} };
            response = out.dump();
            return response.c_str();
        }

           if (req.value("action", "") == "get_revit_id_map") {

            std::string file = req.value("file", "");
            if (file.empty()) { response = R"({"error":"missing file"})"; return response.c_str(); }

            // decode %20 etc. (minimal)
            auto decode = [](const std::string& s) {
                std::string out;
                out.reserve(s.size());
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == '%' && i + 2 < s.size()) {
                        auto hex = [](char c)->int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                            return -1;
                        };
                        int hi = hex(s[i+1]), lo = hex(s[i+2]);
                        if (hi >= 0 && lo >= 0) { out.push_back(char((hi<<4) | lo)); i += 2; continue; }
                    }
                    if (s[i] == '+') { out.push_back(' '); continue; }
                    out.push_back(s[i]);
                }
                return out;
            };

            file = decode(file);

            //std::string path = "/home/puyan_zadeh/storage/outputs/gltf/" + file;
            std::string path = (fs::path(gltf_dir()) / file).string();


            std::ifstream f(path);
            if (!f.is_open()) { response = R"({"error":"revit id map not found"})"; return response.c_str(); }

            response.assign((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            return response.c_str();
        }

        response = R"({"error":"unknown action"})";
    }
    catch (...) {
        response = R"({"error":"invalid input"})";
    }

    return response.c_str();
}

}
