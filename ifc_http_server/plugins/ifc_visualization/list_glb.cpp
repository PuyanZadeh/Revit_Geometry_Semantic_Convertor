// list_glb.cpp
#include "list_glb.h"
#include <filesystem>

std::vector<std::string> list_glb_files(const std::string& folder) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            auto p = entry.path();
            if (p.extension() == ".glb") {
                out.push_back(p.filename().string());
            }
        }
    }
    return out;
}
