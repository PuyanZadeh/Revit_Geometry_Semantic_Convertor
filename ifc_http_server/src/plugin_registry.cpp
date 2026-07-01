#include "plugin_registry.h"
#include <sstream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <dlfcn.h>
#include <iostream>

using json = nlohmann::json;
static const char* kLogPath = "/home/puyan_zadeh/ifc_http_server/logs/ifc_http_server.log";
//static const char* kLogPath = "/home/puyan/home/puyan_zadeh/ifc_http_server/logs/ifc_http_server.log";

void PluginRegistry::auto_scan(const std::string& path) {
    if (FILE* log = fopen(kLogPath, "a")) {
        fprintf(log, "[AutoScan] Scanning path: %s\n", path.c_str());
        fclose(log);
    }

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() != ".so") continue;

        void* handle = dlopen(entry.path().c_str(), RTLD_LAZY);
        if (!handle) {
            if (FILE* log = fopen(kLogPath, "a")) {
                fprintf(log, "[AutoScan] Failed to open: %s\n", entry.path().c_str());
                fclose(log);
            }
            continue;
        }

        auto get_name = (const char* (*)()) dlsym(handle, "plugin_name");
        if (get_name) {
            const char* pname = get_name();
            plugins.push_back({pname, entry.path()});
            if (FILE* log = fopen(kLogPath, "a")) {
                fprintf(log, "[AutoScan] Detected plugin: %s (%s)\n",
                        pname, entry.path().filename().c_str());
                fclose(log);
            }
        } else {
            if (FILE* log = fopen(kLogPath, "a")) {
                fprintf(log, "[AutoScan] No plugin_name symbol in: %s\n", entry.path().c_str());
                fclose(log);
            }
        }
        dlclose(handle);
    }
}

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
}

void PluginRegistry::register_plugin(const std::string& name, const std::string& path) {
    plugins.push_back({name, path});
}

const std::vector<PluginInfo>& PluginRegistry::list() const {
    return plugins;
}

std::string PluginRegistry::to_json() const {
    std::ostringstream out;
    out << R"({"plugins":[)";
    for (size_t i = 0; i < plugins.size(); ++i) {
        out << R"({"name":")" << plugins[i].name
            << R"(","path":")" << plugins[i].path << R"("})";
        if (i + 1 < plugins.size()) out << ",";
    }
    out << "]}";
    return out.str();
}

void PluginRegistry::load_from_config(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) return;

    json j;
    file >> j;
    for (auto& p : j["plugins"]) {
        register_plugin(p["name"], p["path"]);
    }
}
