#include "ifc_convert.h"
#include <array>
#include <cstdio>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <cctype>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const std::string IFC_UPLOAD_DIR = "/home/puyan_zadeh/storage/uploads/ifc/";
static const std::string GLTF_OUTPUT_DIR = "/home/puyan_zadeh/storage/outputs/gltf/";

extern "C"
{
    const char *plugin_name()
    {
        return "IfcConvert Plugin";
    }

    std::string generate_unique_name(const std::string &prefix, const std::string &ext)
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << prefix << "_" << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S");
        return ss.str() + ext;
    }

    /*
        std::string save_uploaded_ifc(const std::string &body)
        {
            fs::create_directories(IFC_UPLOAD_DIR);
            std::string file_path = IFC_UPLOAD_DIR + generate_unique_name("upload", ".ifc");
            std::ofstream f(file_path, std::ios::binary);
            f.write(body.c_str(), body.size());
            f.close();
            return file_path;
        }
    */

    static inline void trim_inplace(std::string &s)
    {
        if (s.empty()) return;

        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;

        size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;

        s = s.substr(b, e - b);
    }

    // call Bonsai Python exporter and return glb path (or empty on error)
    std::string run_bonsai_export_old(const std::string &ifc_path)
    {
        std::ostringstream cmd;
        cmd << "/home/puyan_zadeh/bonsai_venv/bin/python3 "
            << "/home/puyan_zadeh/core/ifcopenshell_bonsai/export_gltf.py "
            << "\"" << ifc_path << "\"";

        std::array<char, 256> buffer{};
        std::string output;

        FILE *pipe = popen(cmd.str().c_str(), "r");
        if (!pipe)
            return "";

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
            output += buffer.data();

        int exit_code = pclose(pipe);
        if (exit_code != 0)
            return "";

        // trim whitespace
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        while (!output.empty() && (output.front() == '\n' || output.front() == '\r' || output.front() == ' '))
            output.erase(0, 1);

        return output;
    }

    std::string run_bonsai_export(const std::string &ifc_path)
    {
        std::ostringstream cmd;
        cmd << "/home/puyan_zadeh/bonsai_venv/bin/python3 "
            << "/home/puyan_zadeh/core/ifcopenshell_bonsai/export_gltf_with_map.py "
            << "\"" << ifc_path << "\"";

        std::array<char, 256> buffer{};
        std::string output;

        FILE *pipe = popen(cmd.str().c_str(), "r");
        if (!pipe)
            return "";

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
            output += buffer.data();

        int exit_code = pclose(pipe);
        if (exit_code != 0)
            return "";

        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        while (!output.empty() && (output.front() == '\n' || output.front() == '\r' || output.front() == ' '))
            output.erase(0, 1);

        return output;
    }

    // -------------------------------------------------------------------------
    // assimp glTF1 -> glTF2
    // -------------------------------------------------------------------------
    std::string run_assimp_convert(const std::string &gltf1_path, std::string &assimp_err)
    {
        std::string gltf2_path = gltf1_path;
        gltf2_path.insert(gltf2_path.find_last_of('.'), "_gltf2");

        std::string cmd =
            "assimp export \"" + gltf1_path + "\" \"" + gltf2_path + "\" 2>&1";

        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
        {
            assimp_err = "assimp: popen failed";
            return "";
        }

        std::string stderr_out;
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe))
            stderr_out += buffer;

        int rc = pclose(pipe);
        if (rc != 0)
        {
            assimp_err = stderr_out;
            return "";
        }

        return gltf2_path;
    }

    // -----------------------------------------------------------------------------
    // SINGLE, AUTHORITATIVE, PERFORMANCE-FIRST SEMANTIC EXPORT
    // Scope: IFCROOT-ish (GlobalId present) but filters out known non-elements
    // Schema: root["elements"][GlobalId] = { step_id, type, raw_attrs }
    // -----------------------------------------------------------------------------
    std::string export_ifc_semantic_json(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        json root;
        root["schema"] = "ai-bim-semantic-v1";
        root["elements"] = json::object();

        std::ifstream f(ifc_path);
        if (!f.is_open())
            return "";

        std::string line;
        while (std::getline(f, line))
        {
            if (line.empty() || line[0] != '#')
                continue;

            size_t eq = line.find('=');
            size_t lp = line.find('(');
            size_t rp = line.rfind(')');

            if (eq == std::string::npos || lp == std::string::npos || rp == std::string::npos)
                continue;

            std::string step_id = line.substr(1, eq - 1);
            std::string type = line.substr(eq + 1, lp - (eq + 1));
            std::string attrs = line.substr(lp + 1, rp - lp - 1);

            trim_inplace(step_id);
            trim_inplace(type);
            trim_inplace(attrs);

            // keep only IFC entities
            if (type.rfind("IFC", 0) != 0)
                continue;

            // reject known non-elements (fast)
            if (type.rfind("IFCREL", 0) == 0 ||
                type.rfind("IFCPROPERTY", 0) == 0 ||
                type.rfind("IFCOWNERHISTORY", 0) == 0 ||
                type.rfind("IFCREPRESENTATION", 0) == 0 ||
                type.rfind("IFCPRODUCTDEFINITIONSHAPE", 0) == 0 ||
                type.rfind("IFCSHAPEREPRESENTATION", 0) == 0 ||
                type.rfind("IFCGEOMETRICREPRESENTATION", 0) == 0)
            {
                continue;
            }

            // GlobalId is first quoted token for IFCROOT-derived instances
            size_t q1 = attrs.find('\'');
            if (q1 == std::string::npos)
                continue;
            size_t q2 = attrs.find('\'', q1 + 1);
            if (q2 == std::string::npos)
                continue;

            std::string globalId = attrs.substr(q1 + 1, q2 - q1 - 1);
            trim_inplace(globalId);
            if (globalId.empty())
                continue;

            root["elements"][globalId] = {
                {"step_id", step_id},
                {"type", type},
                {"raw_attrs", attrs}
            };
        }

        f.close();

        std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return "";
        out << root.dump(2);
        out.close();

        return json_path;
    }

    const char *handle_ifc_convert(const std::string &input_json)
    {
        static std::string response;
        try
        {
            std::string ifc_path;
            std::string ifc_filename;

            // multipart upload: strip wrapper and save IFC
            if (input_json.find("Content-Disposition: form-data") != std::string::npos)
            {
                std::string data = input_json;

                // --- extract original filename from multipart header ---
                // OLD (do not delete)
                // size_t fn_pos = data.find("filename=\"");
                // if (fn_pos != std::string::npos)
                // {
                //     fn_pos += 10;
                //     size_t fn_end = data.find("\"", fn_pos);
                //     ifc_filename = data.substr(fn_pos, fn_end - fn_pos);
                // }

                // NEW: limit search to header section (before first blank line)
                size_t header_end_for_name = data.find("\r\n\r\n");
                if (header_end_for_name == std::string::npos)
                    header_end_for_name = data.find("\n\n");

                std::string header = (header_end_for_name != std::string::npos)
                                         ? data.substr(0, header_end_for_name)
                                         : data;

                size_t fn_pos = header.find("filename=\"");
                if (fn_pos != std::string::npos)
                {
                    fn_pos += 10;
                    size_t fn_end = header.find("\"", fn_pos);
                    if (fn_end != std::string::npos)
                        ifc_filename = header.substr(fn_pos, fn_end - fn_pos);
                }

                size_t header_end = data.find("\r\n\r\n");
                if (header_end == std::string::npos)
                    header_end = data.find("\n\n");
                if (header_end != std::string::npos)
                    data.erase(0, header_end + 4);

                size_t boundary_pos = data.rfind("------");
                if (boundary_pos != std::string::npos)
                    data.erase(boundary_pos);

                while (!data.empty() && (data.front() == '\n' || data.front() == '\r'))
                    data.erase(0, 1);
                while (!data.empty() && (data.back() == '\n' || data.back() == '\r'))
                    data.pop_back();

                fs::create_directories(IFC_UPLOAD_DIR);

                // OLD (do not delete)
                // if (ifc_filename.empty())
                //     ifc_filename = generate_unique_name("upload", ".ifc");
                // // ifc_filename = "test.ifc";

                // NEW: safe fallback if filename missing
                if (ifc_filename.empty())
                {
                    ifc_filename = generate_unique_name("upload", ".ifc");
                }

                // NEW: keep original filename (or fallback)
                ifc_path = IFC_UPLOAD_DIR + ifc_filename;

                std::ofstream f(ifc_path, std::ios::binary | std::ios::trunc);
                f.write(data.c_str(), data.size());
                f.close();
            }
            else
            {
                // JSON: {"file":"/path/to.ifc"}
                json req = json::parse(input_json);
                ifc_path = req.value("file", "");
                if (ifc_path.empty())
                {
                    response = R"({"error":"missing IFC file path"})";
                    return response.c_str();
                }
            }

            std::string gltf_path = run_bonsai_export(ifc_path);
            if (gltf_path.empty())
            {
                response = R"({"plugin":"IfcConvert","status":"error","message":"gltf export failed"})";
                return response.c_str();
            }

            /*
            assimp export \
            ~/storage/outputs/gltf/upload_20251211_211350.glb \
            ~/storage/outputs/gltf/upload_20251211_211350_gltf2.glb
            */

            // FIX: Bonsai returns multiple lines; keep ONLY the GLB path
            size_t nl = gltf_path.find('\n');
            if (nl != std::string::npos)
                gltf_path = gltf_path.substr(0, nl);

            trim_inplace(gltf_path);

            // glTF1 → glTF2 is MANDATORY
            std::string assimp_err;
            std::string gltf2_path = run_assimp_convert(gltf_path, assimp_err);
            if (gltf2_path.empty() || !std::filesystem::exists(gltf2_path))
            {
                response = json({{"plugin", "IfcConvert"},
                                 {"status", "error"},
                                 //{"message", "assimp gltf1 -> gltf2 conversion failed"}})
                                 {"message", assimp_err}})
                               .dump();
                return response.c_str();
            }

            // from here on, ONLY glTF2 is considered valid output
            gltf_path = gltf2_path;

            // existing semantic exports (unchanged)
            /*export_ifc_metadata_json(ifc_path);
            export_ifc_object_index(ifc_path);
            export_ifc_property_data(ifc_path);
            export_ifc_semantic_attributes(ifc_path);
            export_ifc_full_semantics(ifc_path);
            export_ifc_property_sets(ifc_path);*/

            export_ifc_semantic_json(ifc_path);

            // NEW: geometry ↔ semantics join artifact (already produced by Bonsai)
            std::string map_path = gltf_path + ".map.json";

            // DO NOT call export_ifc_geometry_mapping(ifc_path);
            // mapping is authoritative from Bonsai output

            response = json({{"plugin", "IfcConvert"},
                             {"status", "ok"},
                             {"ifc_input", ifc_path},
                             {"gltf_output", gltf_path},
                             {"mapping_file", map_path}})
                           .dump();
        }
        catch (...)
        {
            response = R"({"error":"invalid input or failed conversion"})";
        }
        return response.c_str();
    }

    std::string export_ifc_metadata_json(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        try
        {
            std::ofstream out(json_path, std::ios::binary);
            if (!out.is_open())
                return "";

            std::ifstream in(ifc_path, std::ios::binary);
            if (!in.is_open())
                return "";

            std::string line;
            json metadata;

            while (std::getline(in, line))
            {
                if (line.rfind("FILE_DESCRIPTION", 0) == 0)
                    metadata["description"] = line;
                if (line.rfind("FILE_NAME", 0) == 0)
                    metadata["name"] = line;
                if (line.rfind("FILE_SCHEMA", 0) == 0)
                    metadata["schema"] = line;
            }

            out << metadata.dump(2);
            return json_path;
        }
        catch (...)
        {
            return "";
        }
    }

    std::string export_ifc_object_index(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        // -------------------------
        // load existing JSON
        // -------------------------
        json root;
        {
            std::ifstream in(json_path);
            if (in.good())
                in >> root;
            else
                root = json::object();
        }

        // ensure "objects" array exists
        if (!root.contains("objects"))
            root["objects"] = json::array();

        // -------------------------
        // parse IFC
        // -------------------------
        std::ifstream f(ifc_path);
        if (!f.good())
            return json_path;

        std::string line;
        while (std::getline(f, line))
        {
            if (line.size() < 3 || line[0] != '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string id = line.substr(1, eq - 1);

            size_t type_start = line.find("IFC");
            if (type_start == std::string::npos)
                continue;

            size_t type_end = line.find('(', type_start);
            if (type_end == std::string::npos)
                continue;

            std::string type = line.substr(type_start, type_end - type_start);

            root["objects"].push_back({{"id", id},
                                       {"type", type}});
        }

        // -------------------------
        // write updated JSON back
        // -------------------------
        std::ofstream out(json_path);
        out << root.dump(2);

        return json_path;
    }

    std::string export_ifc_property_data(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        json root;
        {
            std::ifstream in(json_path);
            if (in.good())
                in >> root;
        }
        root["properties"] = json::array();

        std::ifstream f(ifc_path);
        std::string line;
        while (std::getline(f, line))
        {
            if (line.find("IFCPROPERTYSINGLEVALUE") == std::string::npos)
                continue;

            size_t id_start = line.find('#');
            size_t id_end = line.find('=');
            if (id_start == std::string::npos || id_end == std::string::npos)
                continue;
            std::string id = line.substr(id_start + 1, id_end - id_start - 1);

            size_t name_s = line.find("'", id_end);
            if (name_s == std::string::npos)
                continue;
            size_t name_e = line.find("'", name_s + 1);
            if (name_e == std::string::npos)
                continue;
            std::string prop_name = line.substr(name_s + 1, name_e - name_s - 1);

            size_t val_s = line.find("'", name_e + 1);
            if (val_s == std::string::npos)
                continue;
            size_t val_e = line.find("'", val_s + 1);
            if (val_e == std::string::npos)
                continue;
            std::string prop_value = line.substr(val_s + 1, val_e - val_s - 1);

            root["properties"].push_back({{"id", id},
                                          {"name", prop_name},
                                          {"value", prop_value}});
        }

        std::ofstream out(json_path);
        out << root.dump(2);
        return json_path;
    }

    std::string export_ifc_geometry_mapping(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";
        // read existing JSON (metadata + objects)
        std::ifstream in(json_path);
        if (!in.is_open())
            return "";
        json root;
        in >> root;
        in.close();

        // read full IFC file
        std::ifstream ifc_file(ifc_path);
        if (!ifc_file.is_open())
            return "";
        std::string line;

        json mappings = json::array();

        // very simple geometry-mapping pass:
        // look for lines like "#123=IFCSOMETHING(...);" and record minimal refs
        while (std::getline(ifc_file, line))
        {
            if (line.size() < 5 || line[0] != '#')
                continue;

            // extract id
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string id = line.substr(1, eq - 1);

            // extract type
            size_t lp = line.find('(');
            if (lp == std::string::npos)
                continue;
            std::string type = line.substr(eq + 1, lp - (eq + 1));

            // minimal geometry clues: record any placement / shape refs
            bool looks_geom =
                line.find("IFCLOCALPLACEMENT") != std::string::npos ||
                line.find("IFCPRODUCTDEFINITIONSHAPE") != std::string::npos ||
                line.find("IFCSHAPEREPRESENTATION") != std::string::npos ||
                line.find("IFCCARTESIANPOINT") != std::string::npos;

            if (looks_geom)
            {
                json entry;
                entry["id"] = id;
                entry["type"] = type;
                entry["raw"] = line;
                mappings.push_back(entry);
            }
        }

        ifc_file.close();

        // add into same JSON file under new top-level key
        root["geometry_mapping"] = mappings;

        // write back
        std::ofstream out(json_path, std::ios::trunc);
        out << root.dump(2);
        out.close();

        return json_path;
    }

    std::string export_ifc_semantic_attributes(const std::string &ifc_path)
    {
        // derive base name
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        // load existing json
        nlohmann::json root;
        {
            std::ifstream in(json_path);
            if (in.is_open())
            {
                in >> root;
                in.close();
            }
        }

        // ensure objects list exists
        if (!root.contains("objects") || !root["objects"].is_array())
        {
            root["objects"] = nlohmann::json::array();
        }

        std::ifstream file(ifc_path);
        if (!file.is_open())
            return "";

        std::string line;
        // naive semantic extraction
        while (std::getline(file, line))
        {
            if (line.rfind("#", 0) == 0) // starts with '#'
            {
                size_t eq = line.find("=");
                size_t par = line.find("(");

                if (eq == std::string::npos || par == std::string::npos)
                    continue;

                std::string id = line.substr(1, eq - 1);
                std::string type = line.substr(eq + 1, par - (eq + 1));

                // extract raw attributes inside parentheses
                size_t open = line.find("(");
                size_t close = line.rfind(")");

                std::string attrs;
                if (open != std::string::npos && close != std::string::npos && close > open)
                    attrs = line.substr(open + 1, close - open - 1);

                nlohmann::json entry;
                entry["id"] = id;
                entry["type"] = type;
                entry["attrs"] = attrs;

                root["objects"].push_back(entry);
            }
        }

        // write updated json file
        {
            std::ofstream out(json_path);
            out << root.dump(2);
        }

        return json_path;
    }

    // ------------------------------------------------------------
    //  METHOD 4 — Export full IFC semantic attributes
    // ------------------------------------------------------------
    std::string export_ifc_full_semantics(const std::string &ifc_path)
    {
        // json file path (append into existing model JSON)
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        // read IFC
        std::ifstream in(ifc_path);
        if (!in.is_open())
            return "FAILED";
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        // append block
        std::ofstream out(json_path, std::ios::app);
        if (!out.is_open())
            return "FAILED";

        out << ",\n  \"full_semantics\": {\n";
        out << "    \"raw_ifc\": \"IFC_SEMANTIC_EXTRACTION_NOT_IMPLEMENTED_YET\"\n";
        out << "  }\n]\n}\n";

        out.close();
        return json_path;
    }

    std::string export_ifc_property_sets(const std::string &ifc_path)
    {
        std::string base = ifc_path.substr(ifc_path.find_last_of("/\\") + 1);
        base = base.substr(0, base.find_last_of('.'));
        std::string json_path = GLTF_OUTPUT_DIR + base + ".json";

        // read existing json
        json root;
        {
            std::ifstream f(json_path);
            if (f.good())
                f >> root;
            else
                root = json::object();
        }

        json props = json::array();
        std::ifstream f(ifc_path);
        std::string line;

        while (std::getline(f, line))
        {
            // detect lines that belong to a property set or single property
            if (line.find("IFCPROPERTYSINGLEVALUE") != std::string::npos ||
                line.find("IFCPROPERTYSET") != std::string::npos)
            {
                json entry;
                entry["raw"] = line;
                props.push_back(entry);
            }
        }

        root["properties"] = props;

        std::ofstream out(json_path);
        out << root.dump(2);

        return json_path;
    }
}
