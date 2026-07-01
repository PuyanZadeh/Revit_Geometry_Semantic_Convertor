#pragma once
#include <string>
#include <vector>

struct PluginInfo {
    std::string name;
    std::string path;
};

class PluginRegistry {
public:
    static PluginRegistry& instance();
    void register_plugin(const std::string& name, const std::string& path);
    const std::vector<PluginInfo>& list() const;
    std::string to_json() const;
    void load_from_config(const std::string& config_path);
    void auto_scan(const std::string& path);

private:
    PluginRegistry() = default;
    std::vector<PluginInfo> plugins;
};
