#include "aiquery.h"
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>

#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

// ADDED (AI hook)
#include <algorithm>
// ADDED (AI hook)
#include "aiquery_ai_stub.h"

using json = nlohmann::json;

namespace fs = std::filesystem;

static fs::path exe_dir() { return fs::canonical("/proc/self/exe").parent_path(); }
static fs::path server_root() { return exe_dir().parent_path(); }
static fs::path data_root() { return server_root().parent_path() / "storage"; }

static const std::string JSON_DIR = (data_root() / "outputs" / "gltf").string() + "/";
static const std::string kDbgPath = (server_root() / "logs" / "dbg.log").string();

// static const std::string JSON_DIR = "/home/puyan_zadeh/storage/outputs/gltf/";
static const std::string MAPPING_EXT = ".map.json";
static const std::string SEMANTIC_EXT = ".json";
// static const char *kDbgPath = "/home/puyan_zadeh/ifc_http_server/logs/dbg.log";

extern "C"
{
    static void dbg(const char *msg, const std::string &val = "", bool overwrite = false)
    {
        FILE *f = fopen(kDbgPath.c_str(), overwrite ? "w" : "a");
        if (!f)
            return;
        if (val.empty())
            fprintf(f, "%s\n", msg);
        else
            fprintf(f, "%s %s\n", msg, val.c_str());
        fclose(f);
    }

    // static void dbg(const char* msg, const std::string& val = "") {
    //     FILE* f = fopen(kDbgPath, "a");
    //     if (!f) return;
    //     if (val.empty()) fprintf(f, "%s\n", msg);
    //     else fprintf(f, "%s %s\n", msg, val.c_str());
    //     fclose(f);
    // }

    static bool load_json_mmap(const std::string &path, json &out)
    {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;

        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size == 0)
        {
            close(fd);
            return false;
        }

        void *data = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (data == MAP_FAILED)
            return false;

        try
        {
            out = json::parse(
                static_cast<const char *>(data),
                static_cast<const char *>(data) + st.st_size);
        }
        catch (...)
        {
            munmap(data, st.st_size);
            return false;
        }

        munmap(data, st.st_size);
        return true;
    }

    static inline std::string trim_copy(const std::string &s)
    {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            --e;
        return s.substr(b, e - b);
    }

    // ADD THIS HELPER (same file, outside handle_ifc_aiquery)
    static json parse_raw_attrs(const std::string &raw)
    {
        json out = json::object();

        // very fast, zero-regex, positional split
        // IFC raw_attrs format: comma-separated, quoted strings or tokens
        std::vector<std::string> tokens;
        tokens.reserve(16);

        std::string cur;
        bool in_str = false;

        for (char c : raw)
        {
            if (c == '\'')
            {
                in_str = !in_str;
                continue;
            }
            if (c == ',' && !in_str)
            {
                tokens.push_back(trim_copy(cur));
                cur.clear();
            }
            else
                cur.push_back(c);
        }
        if (!cur.empty())
            tokens.push_back(trim_copy(cur));

        // minimal, stable extraction
        if (tokens.size() > 0)
            out["GlobalId"] = tokens[0];
        if (tokens.size() > 2)
            out["Name"] = tokens[2];
        if (tokens.size() > 4)
            out["ObjectType"] = tokens[4];
        if (tokens.size() > 5)
            out["Relating"] = tokens[5];
        if (tokens.size() > 6)
            out["Related"] = tokens[6];

        return out;
    }

    // static nlohmann::json resolve_step_ref(const nlohmann::json &sem_elements, const std::string &step_ref)

    // {
    //     json out = json::object();
    //     if (step_ref.empty() || step_ref[0] != '#')
    //         return out;

    //     const std::string step = step_ref.substr(1);

    //     for (auto &it : sem_elements.items())
    //     {
    //         const json &e = it.value();
    //         if (e.contains("step_id") && e["step_id"].is_string() && e["step_id"] == step)
    //         {
    //             out = e;

    //             // OPTIONAL parse on resolved target
    //             if (e.contains("raw_attrs") && e["raw_attrs"].is_string())
    //             {
    //                 out["parsed"] = parse_raw_attrs(e["raw_attrs"].get<std::string>());
    //             }
    //             out["GlobalId"] = it.key();
    //             break;
    //         }
    //     }
    //     return out;
    // }

    // ADDED: stable helper (no behavior change unless used)
    static inline std::string model_name_from_map_path(const std::string &model_path)
    {
        // model = ".../upload_xxx.map.json"  -> "upload_xxx"
        return fs::path(model_path).stem().stem().string(); // strips ".map.json"
    }


static const json* find_element_by_gid(const json& sem, const std::string& gid)
{
    dbg("FIND_ELEM gid:", gid);

    if (!sem.is_object())
    {
        dbg("FIND_ELEM sem_not_object");
        return nullptr;
    }

    for (auto& group : sem.items())
    {
        const std::string& group_name = group.key();
        const json& bucket = group.value();

        if (!bucket.is_object())
            continue;

        if (!bucket.contains("items") || !bucket["items"].is_array())
            continue;

        const json& items = bucket["items"];

        dbg("FIND_ELEM scanning_bucket:", group_name);
        dbg("FIND_ELEM bucket_size:", std::to_string(items.size()));

        for (const auto& item : items)
        {
            if (!item.is_object())
                continue;

            if (item.contains("uniqueId") && item["uniqueId"].is_string() && item["uniqueId"].get<std::string>() == gid)
            {
                dbg("FIND_ELEM found_in_bucket:", group_name);
                dbg("FIND_ELEM match_field:", "uniqueId");
                return &item;
            }
        }
    }

    dbg("FIND_ELEM not_found_anywhere");
    return nullptr;
}

    // CONDITIONS (explained in-code):
    // 1) If semantic JSON is IFC-style: sem["elements"] is an object keyed by GlobalId -> direct O(1) find().
    // 2) If semantic JSON is your Revit-style grouped buckets: top-level keys like "doors","walls",... each maps to an array;
    //    we scan arrays and match by uniqueId OR revitID OR GlobalId (first hit returns).
    // static const json *find_element_by_gid(const json &sem, const std::string &gid)
    // {
    //     dbg("FIND_ELEM gid:", gid);

    //     if (!sem.is_object())
    //     {
    //         dbg("FIND_ELEM sem_not_object");
    //         return nullptr;
    //     }

    //     // Condition 1: IFC-style { "elements": { "<gid>": {...} } }
    //     if (sem.contains("elements") && sem["elements"].is_object())
    //     {
    //         const json &elements = sem["elements"];
    //         dbg("FIND_ELEM format:", "root.elements");
    //         dbg("FIND_ELEM elements_size:", std::to_string(elements.size()));

    //         auto it = elements.find(gid);
    //         if (it != elements.end())
    //         {
    //             dbg("FIND_ELEM found_in:", "elements");
    //             return &it.value();
    //         }

    //         dbg("FIND_ELEM not_found_in:", "elements");
    //         return nullptr;
    //     }

    //     // Condition 2: Revit-style grouped buckets: { "doors":[...], "walls":[...], ... }
    //     dbg("FIND_ELEM format:", "grouped_buckets");
    //     dbg("FIND_ELEM root_keys_count:", std::to_string(sem.size()));

    //     for (auto &group : sem.items())
    //     {
    //         const std::string &group_name = group.key();
    //         const json &bucket = group.value();

    //         if (!bucket.is_array())
    //             continue;

    //         dbg("FIND_ELEM scanning_bucket:", group_name);
    //         dbg("FIND_ELEM bucket_size:", std::to_string(bucket.size()));

    //         for (const auto &item : bucket)
    //         {
    //             if (!item.is_object())
    //                 continue;

    //             if (item.contains("uniqueId") && item["uniqueId"].is_string() && item["uniqueId"].get<std::string>() == gid)
    //             {
    //                 dbg("FIND_ELEM found_in_bucket:", group_name);
    //                 dbg("FIND_ELEM match_field:", "uniqueId");
    //                 return &item;
    //             }

    //             if (item.contains("revitID") && item["revitID"].is_string() && item["revitID"].get<std::string>() == gid)
    //             {
    //                 dbg("FIND_ELEM found_in_bucket:", group_name);
    //                 dbg("FIND_ELEM match_field:", "revitID");
    //                 return &item;
    //             }

    //             if (item.contains("GlobalId") && item["GlobalId"].is_string() && item["GlobalId"].get<std::string>() == gid)
    //             {
    //                 dbg("FIND_ELEM found_in_bucket:", group_name);
    //                 dbg("FIND_ELEM match_field:", "GlobalId");
    //                 return &item;
    //             }
    //         }
    //     }

    //     dbg("FIND_ELEM not_found_anywhere");
    //     return nullptr;
    // }

    const char *plugin_name()
    {
        return "AI Query Plugin";
    }

    // forward decl so helpers can re-dispatch
    const char *handle_ifc_aiquery(const std::string &input_json);

    static bool handle_action_nl_query(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "nl_query")
            return false;

        /* ---------------- NL QUERY (added; existing actions unchanged) ---------------- */
        std::string nl = req.value("nl", "");
        if (nl.empty())
        {
            response = R"({"error":"missing nl"})";
            return true;
        }

        // AI returns AST (placeholder for now)
        json ast = ai_nl_to_ast(nl);
        std::string op = ast.value("op", "");

        // deterministic dispatch to existing handlers
        if (op == "count_by_type")
        {
            json fwd = {
                {"action", "count_by_type"},
                {"type", ast.value("type", "")},
                {"model", model}};

            response = handle_ifc_aiquery(fwd.dump());
            return true;
        }

        if (op == "validate_id")
        {
            json fwd = {
                {"action", "validate_id"},
                {"globalId", ast.value("globalId", "")},
                {"model", model}};
            response = handle_ifc_aiquery(fwd.dump());
            return true;
        }

        if (op == "query_model")
        {
            json fwd = {
                {"action", "query_model"},
                {"query", ast.value("globalId", "")},
                {"model", model}};
            response = handle_ifc_aiquery(fwd.dump());
            return true;
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "error"},
                         {"error", "unsupported op"},
                         {"ast", ast}})
                       .dump();
        return true;
    }

    static bool handle_action_list_models(const json &req, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "list_models")
            return false;

        /* ---------------- list models ---------------- */
        std::ostringstream oss;
        oss << "ls " << JSON_DIR << "*" << MAPPING_EXT;

        FILE *pipe = popen(oss.str().c_str(), "r");
        if (!pipe)
        {
            response = R"({"error":"failed to list models"})";
            return true;
        }

        json files = json::array();
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe))
        {
            std::string f(buffer);
            f.erase(f.find_last_not_of(" \n\r\t") + 1);
            files.push_back(f);
        }
        pclose(pipe);

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"models", files}})
                       .dump();
        return true;
    }

    static bool handle_action_query_model(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "query_model")
            return false;

        /* ---------------- GlobalId → node indices ---------------- */
        std::string gid = req.value("query", "");

        // OLD
        // std::string map_path = JSON_DIR + model + MAPPING_EXT;

        // NEW: model is already full path to .map.json
        std::string map_path = model;

        std::ifstream in(map_path);
        if (!in.good())
        {
            response = json({{"error", "mapping file not found"},
                             {"path", map_path}})
                           .dump();
            return true;
        }

        json mapping;
        in >> mapping;
        in.close();

        json matches = json::array();
        auto it = mapping.find(gid);
        if (it != mapping.end())
        {
            matches.push_back({{"GlobalId", it.key()},
                               {"node_indices", it.value()}});
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"matches", matches}})
                       .dump();
        return true;
    }

    static bool handle_action_count_by_type(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "count_by_type")
            return false;

        /* ---------------- count by IFC type ---------------- */
        std::string type = req.value("type", "");

        // OLD
        // std::string sem_path = JSON_DIR + model + SEMANTIC_EXT;

        // NEW: derive base model name from full path
        // std::string model_name = fs::path(model).stem().string();
        std::string model_name = model_name_from_map_path(model); // strips ".map.json"
        std::string sem_path = JSON_DIR + model_name + SEMANTIC_EXT;

        if (type.empty())
        {
            response = R"({"error":"missing type"})";
            return true;
        }

        // OBSOLETE (slower)
        // std::ifstream in(sem_path);
        // if (!in.good())
        // {
        //     response = json({{"error", "semantic json not found"},
        //                      {"path", sem_path}})
        //                    .dump();
        //     return response.c_str();
        // }
        // json sem;
        // in >> sem;
        // in.close();

        json sem;
        if (!load_json_mmap(sem_path, sem))
        {
            response = json({{"error", "semantic json not found"},
                             {"path", sem_path}})
                           .dump();
            return true;
        }

        size_t count = 0;
        const json &elements = sem["types"]["items"];

        for (const auto &el : elements)
        {
            if (!el.is_object())
                continue;

            if (el.value("type", "") != type)
                continue;

            bool pass = true;

            if (req.contains("filters") && req["filters"].is_array())
            {
                const json &params = (el.contains("parameters") && el["parameters"].is_object())
                                         ? el["parameters"]
                                         : json::object();

                for (const auto &f : req["filters"])
                {
                    const std::string field = f.value("field", "");
                    const std::string value = f.value("value", "");

                    if (!params.contains(field) || !params[field].is_string() || params[field].get<std::string>() != value)
                    {
                        pass = false;
                        break;
                    }
                }
            }

            if (pass)
                count++;
        }
        /*
                if (sem.contains("elements") && sem["elements"].is_object())        {
                    for (auto &el : sem["elements"].items()){
                        if (el.value().value("type", "") != type)
                            continue;

                        bool pass = true;

                        // APPLY FILTERS (optional)
                        if (req.contains("filters") && req["filters"].is_array())
                        {
                            for (auto &f : req["filters"])
                            {
                                const std::string field = f.value("field", "");
                                const std::string value = f.value("value", "");

                                if (!el.value().contains(field) ||
                                    el.value()[field].get<std::string>() != value)
                                {
                                    pass = false;
                                    break;
                                }
                            }
                        }

                        if (pass)
                            count++;
                    }
                }
        */
        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"type", type},
                         {"count", count}})
                       .dump();
        return true;
    }

    static bool handle_action_search_elements(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "search_elements")
            return false;

        /* ---------------- search elements ---------------- */
        /*
        Request:
        {
          "action": "search_elements",
          "model": "/full/path/to/model.map.json",
          "type": "IFCWALL",          // optional
          "name_contains": "Wall",    // optional
          "limit": 50                // optional
        }
        */

        std::string type_filter = req.value("type", "");
        std::string name_filter = req.value("name_contains", "");
        size_t limit = req.value("limit", 100);

        // derive semantic path from map.json
        std::string model_name = fs::path(model).stem().stem().string(); // strips .map.json
        std::string sem_path = JSON_DIR + model_name + SEMANTIC_EXT;

        json sem;
        if (!load_json_mmap(sem_path, sem))
        {
            response = json({{"error", "semantic json not found or invalid"},
                             {"path", sem_path}})
                           .dump();
            return true;
        }

        json results = json::array();

        // NOTE: this action is still IFC-oriented (expects sem["elements"]); kept as-is except for fallback.
        // const json &elements = (sem.contains("elements") && sem["elements"].is_object()) ? sem["elements"] : sem;
        const json &elements = sem["types"]["items"];

for (const auto& elem : elements)
{
    if (results.size() >= limit)
        break;

    if (!elem.is_object())
        continue;

    const std::string gid = elem.value("uniqueId", "");

    // type filter
    if (!type_filter.empty())
    {
        if (elem.value("type", "") != type_filter)
            continue;
    }

    // name filter via parameters.Type Name
    if (!name_filter.empty())
    {
        if (!elem.contains("parameters") || !elem["parameters"].is_object())
            continue;

        const json& params = elem["parameters"];

        if (!params.contains("Type Name") || 
            !params["Type Name"].is_string() ||
            params["Type Name"].get<std::string>().find(name_filter) == std::string::npos)
            continue;
    }

    json entry;
    entry["GlobalId"] = gid;
    entry["type"] = elem.value("type", "");

    results.push_back(entry);
}
        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"count", results.size()},
                         {"results", results}})
                       .dump();

        return true;
    }

    static bool handle_action_get_element(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "get_element")
            return false;

        /* ---------------- get element (semantic + properties + geometry) ---------------- */
        std::string gid = req.value("globalId", "");
        if (gid.empty())
        {
            response = R"({"error":"missing globalId"})";
            return true;
        }

        // CONDITIONS (explained in-code, no hardcoded paths):
        // model is expected to be a file name like "X.map.json" (from UI), so we join JSON_DIR here.
        // semantic file is expected to be "X.json" where X is model without ".map.json".
        std::string map_path = JSON_DIR + model;
        std::string model_name = model_name_from_map_path(model);
        std::string sem_path = JSON_DIR + model_name + SEMANTIC_EXT;

        dbg("Starting", "Debugging", true);
        dbg("GET_ELEMENT map_path:", map_path);
        dbg("GET_ELEMENT model_name:", model_name);
        dbg("GET_ELEMENT sem_path:", sem_path);

        bool semantic_ok = false;
        bool geometry_ok = false;

        json element = json::object();
        json properties = json::object();
        json nodes = json::array();

        // semantic
        json sem;
        bool sem_loaded = load_json_mmap(sem_path, sem);
        dbg("GET_ELEMENT sem_load:", sem_loaded ? "true" : "false");

        if (sem_loaded)
        {
            dbg("inside load_json_mmap()");
            dbg("GET_ELEMENT elements_size:", sem.contains("elements") ? std::to_string(sem["elements"].size()) : "no_elements");
            dbg("GET_ELEMENT has_gid:", (sem.contains("elements") && sem["elements"].is_object() && sem["elements"].contains(gid)) ? "true" : "false");

            const json *found_elem = find_element_by_gid(sem, gid);

            dbg("gid: ", gid);

            if (found_elem)
            {
                dbg("element or it.value(): ", found_elem->dump());
                semantic_ok = true;
                element = *found_elem;

                dbg("element or it.value(): ", element.dump());

                if (element.contains("raw_attrs") && element["raw_attrs"].is_string())
                {
                    properties = parse_raw_attrs(element["raw_attrs"].get<std::string>());

                    // resolve STEP refs (#xxxx) → actual elements
                    // This only makes sense for IFC-style sem["elements"]; for bucketed Revit JSON there is no step_id index map.
                    /*if (sem.contains("elements") && sem["elements"].is_object())
                    {
                        const json &elements = sem["elements"];

                        if (properties.contains("Relating") && properties["Relating"].is_string())
                            properties["Relating_resolved"] =
                                resolve_step_ref(elements, properties["Relating"].get<std::string>());

                        if (properties.contains("Related") && properties["Related"].is_string())
                            properties["Related_resolved"] =
                                resolve_step_ref(elements, properties["Related"].get<std::string>());
                    }*/
                   /*if (sem.contains("elements") && sem["elements"].is_object())
{
    const json &elements = sem["elements"];

    if (properties.contains("Relating") && properties["Relating"].is_string())
        properties["Relating_resolved"] =
            resolve_step_ref(elements, properties["Relating"].get<std::string>());

    if (properties.contains("Related") && properties["Related"].is_string())
        properties["Related_resolved"] =
            resolve_step_ref(elements, properties["Related"].get<std::string>());
}*/
                }
            }
            else
            {
                dbg("Never found gid");
            }
        }
        else
        {
            dbg("did not get inside load_json_mmap()");
        }

        // geometry
        std::ifstream min(map_path);
        dbg("GET_ELEMENT map_open:", min.good() ? "true" : "false");

        if (min.good())
        {
            json map;
            min >> map;
            min.close();

            auto it = map.find(gid);
            if (it != map.end())
            {
                geometry_ok = true;
                nodes = it.value();
            }
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"GlobalId", gid},
                         {"semantic", semantic_ok},
                         {"geometry", geometry_ok},
                         {"node_indices", nodes},
                         {"element", element},
                         {"properties", properties}})
                       .dump();

        return true;
    }
    static bool handle_action_validate_id(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "validate_id")
            return false;

        std::string gid = req.value("globalId", "");
        if (gid.empty())
        {
            response = R"({"error":"missing globalId"})";
            return true;
        }

        json properties = json::object();

        std::string model_name = fs::path(model).stem().stem().string();
        std::string sem_path = JSON_DIR + model_name + SEMANTIC_EXT;

        dbg("Starting", "Debugging", true);

        bool semantic_ok = false;

        json sem;
        if (load_json_mmap(sem_path, sem))
        {
            if (sem.is_object())
            {
                for (auto &group : sem.items())
                {
                    if (group.key() == "types")
                        continue;

                    const json &bucket = group.value();
                    if (!bucket.is_object())
                        continue;

                    if (!bucket.contains("items") || !bucket["items"].is_array())
                        continue;

                    const json &items = bucket["items"];

                    for (const auto &item : items)
                    {
                        if (!item.is_object())
                            continue;

                        if (item.contains("uniqueId") && item["uniqueId"].is_string() && item["uniqueId"].get<std::string>() == gid)
                        {
                            semantic_ok = true;

                            if (item.contains("parameters") && item["parameters"].is_object())
                                properties = item["parameters"];
                            else
                                properties = json::object();

                            break;
                        }
                    }

                    if (semantic_ok)
                        break;
                }
            }
        }

        std::string map_path = model;

        std::ifstream min(map_path);
        bool geometry_ok = false;
        json nodes = json::array();

        if (min.good())
        {
            json map;
            min >> map;
            min.close();

            auto it = map.find(gid);
            if (it != map.end())
            {
                geometry_ok = true;
                nodes = it.value();
            }
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"GlobalId", gid},
                         {"semantic", semantic_ok},
                         {"properties", properties},
                         {"geometry", geometry_ok},
                         {"node_indices", nodes}})
                       .dump();

        return true;
    }
    // static bool handle_action_validate_id(const json &req, const std::string &model, std::string &response)
    // {
    //     std::string action = req.value("action", "");
    //     if (action != "validate_id")
    //         return false;

    //     /* ---------------- validate GlobalId ---------------- */
    //     std::string gid = req.value("globalId", "");
    //     if (gid.empty())
    //     {
    //         response = R"({"error":"missing globalId"})";
    //         return true;
    //     }

    //     json properties = json::object();

    //     /* ---------- semantic lookup ---------- */

    //     // OLD
    //     // std::string model_name = fs::path(model).filename().string();
    //     // model_name = model_name.substr(0, model_name.find(".glb"));

    //     // NEW: robust + consistent with mapping logic

    //     /* ---------- semantic lookup ---------- */

    //     // NEW: robust + consistent with mapping logic
    //     // std::string model_name = fs::path(model).stem().string();
    //     std::string model_name = fs::path(model).stem().stem().string(); // strips ".map.json"

    //     std::string sem_path = JSON_DIR + model_name + SEMANTIC_EXT;

    //     dbg("Starting", "Debugging", true);
    //     // dbg("SEM_PATH", sem_path, false);

    //     bool semantic_ok = false;

    //     // std::ifstream sin(sem_path);
    //     // if (sin.good())
    //     // {
    //     //     json sem;
    //     //     sin >> sem;
    //     //     sin.close();
    //     json sem;
    //     if (load_json_mmap(sem_path, sem))
    //     {

    //         // dbg("SEM_ROOT_KEYS", std::to_string(sem.size()), false);

    //         // REQUIRED: semantics live under sem["elements"]
    //         // if (sem.contains("elements") && sem["elements"].is_object())    {
    //         if (!sem.contains("elements") || !sem["elements"].is_object())
    //         {
    //             // dbg("SEM_ERROR", "missing elements object", false);
    //             semantic_ok = false;
    //         }
    //         else
    //         {
    //             // auto &elements = sem["elements"];
    //             // dbg("", std::to_string(elements.size()));
    //             // dbg("SEM_ELEMENTS_COUNT", std::to_string(sem["elements"].size()), false);
    //             auto it = sem["elements"].find(gid);
    //             if (it != sem["elements"].end())
    //             {
    //                 semantic_ok = true;

    //                 // std::string prop_path = JSON_DIR + model_name + ".props.json"; // or your actual property file
    //                 // json properties = json::object();
    //                 const json &elem = it.value();

    //                 dbg("SEM_Prop_", "passed extracting the props", false);
    //                 // properties are embedded in semantic JSON → raw_attrs
    //                 if (elem.contains("raw_attrs") && elem["raw_attrs"].is_string())
    //                 {
    //                     dbg("SEM_Prop_", "inside the IF raw_attrs", false);

    //                     properties = parse_raw_attrs(elem["raw_attrs"].get<std::string>());

    //                     dbg("SEM_Prop_", "extracted the raw_attrs", false);

    //                     // resolve STEP refs (#xxxx) → actual elements
    //                     if (properties.contains("Relating") && properties["Relating"].is_string())
    //                         properties["Relating_resolved"] =
    //                             resolve_step_ref(sem["elements"], properties["Relating"].get<std::string>());

    //                     if (properties.contains("Related") && properties["Related"].is_string())
    //                         properties["Related_resolved"] =
    //                             resolve_step_ref(sem["elements"], properties["Related"].get<std::string>());
    //                 }
    //             }

    //             /* // Direct O(1) lookup by GlobalId
    //             // if (elements.contains(gid))
    //             // if (sem["elements"].contains(gid))
    //             // {
    //             //     semantic_ok = true;
    //             //     // dbg("SEM_MATCH contain", gid, false);
    //             // }
    //             // else
    //             // {
    //             // dbg("SEM_NO_MATCH : contain faild", gid, false);
    //             // } */
    //         }
    //     }

    //     /* ---------- geometry lookup ---------- */

    //     // OLD
    //     // std::string map_path = JSON_DIR + model + MAPPING_EXT;

    //     // NEW: model already full path
    //     std::string map_path = model;

    //     std::ifstream min(map_path);
    //     bool geometry_ok = false;
    //     json nodes = json::array();

    //     if (min.good())
    //     {
    //         json map;
    //         min >> map;
    //         min.close();

    //         auto it = map.find(gid);
    //         if (it != map.end())
    //         {
    //             geometry_ok = true;
    //             nodes = it.value();
    //         }
    //     }

    //     response = json({{"plugin", "AIQuery"},
    //                      {"status", "ok"},
    //                      {"GlobalId", gid},
    //                      {"semantic", semantic_ok},
    //                      {"properties", properties},
    //                      {"geometry", geometry_ok},
    //                      {"node_indices", nodes}})
    //                    .dump();

    //     return true;
    // }

    const char *handle_ifc_aiquery(const std::string &input_json)
    {
        static std::string response;

        try
        {
            json req = json::parse(input_json);
            std::string action = req.value("action", "");
            std::string model = req.value("model", "");

            if (handle_action_nl_query(req, model, response))
                return response.c_str();

            if (handle_action_list_models(req, response))
                return response.c_str();

            if (handle_action_query_model(req, model, response))
                return response.c_str();

            if (handle_action_count_by_type(req, model, response))
                return response.c_str();

            if (handle_action_search_elements(req, model, response))
                return response.c_str();

            if (handle_action_get_element(req, model, response))
                return response.c_str();

            if (handle_action_validate_id(req, model, response))
                return response.c_str();

            response = json({{"plugin", "AIQuery"},
                             {"status", "idle"}})
                           .dump();
        }
        catch (...)
        {
            response = R"({"error":"invalid input or runtime error"})";
        }

        return response.c_str();
    }
}