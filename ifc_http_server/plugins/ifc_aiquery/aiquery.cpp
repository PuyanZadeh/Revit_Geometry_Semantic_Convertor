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

#include <algorithm>
#include "aiquery_ai_stub.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

static fs::path exe_dir() { return fs::canonical("/proc/self/exe").parent_path(); }
static fs::path server_root() { return exe_dir().parent_path(); }
static fs::path data_root() { return server_root().parent_path() / "storage"; }

static const std::string JSON_DIR = (data_root() / "outputs" / "gltf").string() + "/";
static const std::string kDbgPath = (server_root() / "logs" / "dbg.log").string();

static const std::string MAPPING_EXT = ".map.json";
static const std::string SEMANTIC_EXT = ".json";

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

    static inline std::string model_name_from_map_path(const std::string &model_path)
    {
        return fs::path(model_path).stem().stem().string();
    }

    static inline std::string resolve_map_path(const std::string &model)
    {
        fs::path p(model);

        if (p.is_absolute())
            return p.string();

        return JSON_DIR + model;
    }

    static inline std::string resolve_semantic_path_from_model(const std::string &model)
    {
        return JSON_DIR + model_name_from_map_path(model) + SEMANTIC_EXT;
    }

    static bool json_contains_node_index(const json &value, int64_t node_index)
    {
        if (value.is_number_integer())
            return value.get<int64_t>() == node_index;

        if (value.is_array())
        {
            for (const auto &v : value)
            {
                if (json_contains_node_index(v, node_index))
                    return true;
            }
        }

        return false;
    }

    static bool parse_int64(const std::string &s, int64_t &out)
    {
        try
        {
            size_t pos = 0;
            long long v = std::stoll(s, &pos);
            if (pos != s.size())
                return false;

            out = static_cast<int64_t>(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static std::string resolve_unique_id_from_map(const json &map, const std::string &clicked_id, int64_t node_index = -1)
    {
        if (!map.is_object())
            return "";

        if (!clicked_id.empty() && map.contains(clicked_id))
            return clicked_id;

        int64_t parsed_node_index = -1;
        bool has_parsed_node_index = parse_int64(clicked_id, parsed_node_index);

        for (const auto &it : map.items())
        {
            const std::string &unique_id = it.key();
            const json &nodes = it.value();

            if (node_index >= 0 && json_contains_node_index(nodes, node_index))
                return unique_id;

            if (has_parsed_node_index && json_contains_node_index(nodes, parsed_node_index))
                return unique_id;
        }

        return "";
    }

    static const json *find_semantic_by_unique_id(const json &sem, const std::string &uniqueId)
    {
        if (!sem.is_object())
            return nullptr;

        for (const auto &group : sem.items())
        {
            const json &bucket = group.value();

            if (!bucket.is_object())
                continue;

            if (!bucket.contains("items") || !bucket["items"].is_array())
                continue;

            for (const auto &item : bucket["items"])
            {
                if (!item.is_object())
                    continue;

                if (item.value("uniqueId", "") == uniqueId)
                    return &item;
            }
        }

        return nullptr;
    }

    static const json *find_next_semantic_context(const json &sem, const json &element)
    {
        if (!element.is_object())
            return nullptr;

        if (element.contains("hostId") && element["hostId"].is_string())
            return find_semantic_by_unique_id(sem, element["hostId"].get<std::string>());

        return nullptr;
    }

    const char *plugin_name()
    {
        return "AI Query Plugin";
    }

    const char *handle_ifc_aiquery(const std::string &input_json);

    static bool handle_action_nl_query(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "nl_query")
            return false;

        std::string nl = req.value("nl", "");
        if (nl.empty())
        {
            response = R"({"error":"missing nl"})";
            return true;
        }

        json ast = ai_nl_to_ast(nl);
        std::string op = ast.value("op", "");

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

        std::string query = req.value("query", "");
        int64_t node_index = req.value("nodeIndex", -1);

        std::string map_path = resolve_map_path(model);

        std::ifstream in(map_path);
        if (!in.good())
        {
            response = json({{"error", "mapping file not found"},
                             {"path", map_path}})
                           .dump();
            return true;
        }

        json map;
        in >> map;
        in.close();

        std::string unique_id = resolve_unique_id_from_map(map, query, node_index);

        json matches = json::array();

        if (!unique_id.empty() && map.contains(unique_id))
        {
            matches.push_back({{"uniqueId", unique_id},
                               {"node_indices", map[unique_id]}});
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"query", query},
                         {"resolved_uniqueId", unique_id},
                         {"matches", matches}})
                       .dump();

        return true;
    }

    static bool handle_action_count_by_type(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "count_by_type")
            return false;

        std::string type = req.value("type", "");
        if (type.empty())
        {
            response = R"({"error":"missing type"})";
            return true;
        }

        std::string sem_path = resolve_semantic_path_from_model(model);

        json sem;
        if (!load_json_mmap(sem_path, sem))
        {
            response = json({{"error", "semantic json not found"},
                             {"path", sem_path}})
                           .dump();
            return true;
        }

        size_t count = 0;

        if (sem.is_object())
        {
            for (const auto &group : sem.items())
            {
                const json &bucket = group.value();

                if (!bucket.is_object())
                    continue;

                if (!bucket.contains("items") || !bucket["items"].is_array())
                    continue;

                for (const auto &el : bucket["items"])
                {
                    if (!el.is_object())
                        continue;

                    bool type_match = false;

                    if (el.value("type", "") == type)
                        type_match = true;

                    if (el.contains("parameters") && el["parameters"].is_object())
                    {
                        const json &params = el["parameters"];

                        if (params.value("Type", "") == type)
                            type_match = true;

                        if (params.value("Type Name", "") == type)
                            type_match = true;
                    }

                    if (!type_match)
                        continue;

                    bool pass = true;

                    if (req.contains("filters") && req["filters"].is_array())
                    {
                        for (const auto &f : req["filters"])
                        {
                            const std::string field = f.value("field", "");
                            const std::string value = f.value("value", "");

                            if (!el.contains("parameters") || !el["parameters"].is_object())
                            {
                                pass = false;
                                break;
                            }

                            const json &params = el["parameters"];

                            if (!params.contains(field) ||
                                !params[field].is_string() ||
                                params[field].get<std::string>() != value)
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
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"type", type},
                         {"count", count}})
                       .dump();

        return true;
    }

    // ------------------------------------------------------------------
    // Generic semantic filtering engine (Semantic -> Semantic, Step 1)
    //
    // Everything below operates on plain JSON keys/values only. Nothing here
    // may branch on a category name (Doors/Walls/Rooms/...) or a specific
    // field name (Fire Rating/Width/...). New exporter fields and new
    // top-level categories are supported automatically because lookup is by
    // whatever key name the caller asks for, not by an enumerated list.
    // ------------------------------------------------------------------

    // Field resolution precedence (documented per spec requirement 3):
    //   1. The field name as a direct top-level key on the semantic object
    //      (category, uniqueId, typeId, parentId, hostId, levelId, intId,
    //      refs, or any future top-level field the exporter adds).
    //   2. If not found at the top level, the same key name inside the
    //      object's "parameters" map (Revit parameters: "Fire Rating",
    //      "Type", "Name", "Manufacturer", "Width", ...).
    // Both steps are plain key lookups; no field name is ever special-cased.
    static const json *resolve_field(const json &item, const std::string &field)
    {
        if (!item.is_object() || field.empty())
            return nullptr;

        auto top = item.find(field);
        if (top != item.end())
            return &(*top);

        auto params_it = item.find("parameters");
        if (params_it != item.end() && params_it->is_object())
        {
            auto p = params_it->find(field);
            if (p != params_it->end())
                return &(*p);
        }

        return nullptr;
    }

    // Generic stringification used by eq/neq/contains. Works uniformly for
    // any JSON value type a field might hold, with no field-specific parsing.
    static std::string json_to_compare_string(const json &v)
    {
        if (v.is_string())
            return v.get<std::string>();

        if (v.is_null())
            return "";

        if (v.is_boolean())
            return v.get<bool>() ? "true" : "false";

        if (v.is_number_integer())
            return std::to_string(v.get<int64_t>());

        if (v.is_number_unsigned())
            return std::to_string(v.get<uint64_t>());

        if (v.is_number_float())
        {
            std::ostringstream oss;
            oss << v.get<double>();
            return oss.str();
        }

        return v.dump();
    }

    // Generic leading-numeric-token parse, e.g. pulls 90 out of "90 minutes"
    // or 4 out of "4' - 5 1/2\"". This is a plain text->number parse, NOT a
    // unit converter: it never interprets units, feet/inches, or suffixes.
    // Step 1 intentionally stops here; unit normalization (e.g. mm vs ft-in)
    // is out of scope and left for a later step.
    static bool extract_leading_number(const std::string &s, double &out)
    {
        size_t i = 0, n = s.size();

        while (i < n && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;

        size_t start = i;

        if (i < n && (s[i] == '+' || s[i] == '-'))
            ++i;

        bool has_digits = false;

        while (i < n && std::isdigit(static_cast<unsigned char>(s[i])))
        {
            ++i;
            has_digits = true;
        }

        if (i < n && s[i] == '.')
        {
            ++i;
            while (i < n && std::isdigit(static_cast<unsigned char>(s[i])))
            {
                ++i;
                has_digits = true;
            }
        }

        if (!has_digits)
            return false;

        try
        {
            out = std::stod(s.substr(start, i - start));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Generic numeric coercion for gt/gte/lt/lte. Same rule for every field:
    // native JSON numbers are used directly, strings are parsed via
    // extract_leading_number (token parse only, no unit conversion).
    static bool json_value_to_number(const json &v, double &out)
    {
        if (v.is_number())
        {
            out = v.get<double>();
            return true;
        }

        if (v.is_boolean())
        {
            out = v.get<bool>() ? 1.0 : 0.0;
            return true;
        }

        if (v.is_string())
            return extract_leading_number(v.get<std::string>(), out);

        return false;
    }

    // Evaluates a single {field, op, value} clause against one semantic
    // object. Dispatches purely on the "op" string; contains no field-name
    // or category-name branching.
    static bool eval_filter_clause(const json &item, const json &clause)
    {
        if (!clause.is_object())
            return false;

        std::string field = clause.value("field", "");
        std::string op = clause.value("op", "eq");

        if (field.empty())
            return false;

        const json *actual = resolve_field(item, field);

        if (op == "exists")
            return actual != nullptr && !actual->is_null();

        // Every other operator requires an actual, non-null value to compare
        // against. A missing field never satisfies eq/neq/contains/gt/gte/lt/lte;
        // callers that need to distinguish "absent" should use "exists".
        if (!actual || actual->is_null())
            return false;

        json expected = clause.contains("value") ? clause.at("value") : json(nullptr);

        if (op == "eq")
            return json_to_compare_string(*actual) == json_to_compare_string(expected);

        if (op == "neq")
            return json_to_compare_string(*actual) != json_to_compare_string(expected);

        if (op == "contains")
        {
            std::string needle = json_to_compare_string(expected);
            if (needle.empty())
                return false;

            return json_to_compare_string(*actual).find(needle) != std::string::npos;
        }

        if (op == "gt" || op == "gte" || op == "lt" || op == "lte")
        {
            double a = 0.0, b = 0.0;

            // Step 1 numeric comparison is a generic numeric-token parse only
            // (see extract_leading_number); it does not do unit conversion.
            if (!json_value_to_number(*actual, a) || !json_value_to_number(expected, b))
                return false;

            if (op == "gt")
                return a > b;

            if (op == "gte")
                return a >= b;

            if (op == "lt")
                return a < b;

            return a <= b;
        }

        // Unknown operator: never matches, rather than throwing.
        return false;
    }

    // Combines all clauses for one semantic object using a generic AND/OR
    // fold. "match" is "all" (AND, default) or "any" (OR); the fold itself
    // has no knowledge of what each clause checks.
    static bool eval_filters(const json &item, const json &filters, const std::string &match)
    {
        if (!filters.is_array() || filters.empty())
            return true;

        bool require_all = (match != "any");

        for (const auto &clause : filters)
        {
            bool clause_result = eval_filter_clause(item, clause);

            if (require_all && !clause_result)
                return false;

            if (!require_all && clause_result)
                return true;
        }

        return require_all;
    }

    static bool handle_action_semantic_filter(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "semantic_filter")
            return false;

        std::string sem_path = resolve_semantic_path_from_model(model);

        json sem;
        if (!load_json_mmap(sem_path, sem))
        {
            response = json({{"error", "semantic json not found"},
                             {"path", sem_path}})
                           .dump();
            return true;
        }

        json filters = (req.contains("filters") && req["filters"].is_array())
                           ? req["filters"]
                           : json::array();

        std::string match = req.value("match", "all");

        json results = json::array();

        // Generic walk: every top-level bucket, every item inside its
        // "items" array, regardless of bucket name or item category.
        if (sem.is_object())
        {
            for (const auto &bucket_entry : sem.items())
            {
                const json &bucket = bucket_entry.value();

                if (!bucket.is_object() || !bucket.contains("items") || !bucket["items"].is_array())
                    continue;

                for (const auto &item : bucket["items"])
                {
                    if (!item.is_object())
                        continue;

                    if (eval_filters(item, filters, match))
                        results.push_back(item);
                }
            }
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"action", "semantic_filter"},
                         {"match", match},
                         {"filters", filters},
                         {"count", results.size()},
                         {"results", results}})
                       .dump();

        return true;
    }

    static bool handle_action_search_elements(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "search_elements")
            return false;

        std::string type_filter = req.value("type", "");
        std::string name_filter = req.value("name_contains", "");
        size_t limit = req.value("limit", 100);

        std::string sem_path = resolve_semantic_path_from_model(model);

        json sem;
        if (!load_json_mmap(sem_path, sem))
        {
            response = json({{"error", "semantic json not found or invalid"},
                             {"path", sem_path}})
                           .dump();
            return true;
        }

        json results = json::array();

        if (sem.is_object())
        {
            for (const auto &group : sem.items())
            {
                if (results.size() >= limit)
                    break;

                const std::string &group_name = group.key();
                const json &bucket = group.value();

                if (!bucket.is_object())
                    continue;

                if (!bucket.contains("items") || !bucket["items"].is_array())
                    continue;

                for (const auto &elem : bucket["items"])
                {
                    if (results.size() >= limit)
                        break;

                    if (!elem.is_object())
                        continue;

                    if (!type_filter.empty())
                    {
                        bool type_match = false;

                        if (elem.value("type", "") == type_filter)
                            type_match = true;

                        if (elem.contains("parameters") && elem["parameters"].is_object())
                        {
                            const json &params = elem["parameters"];

                            if (params.value("Type", "") == type_filter)
                                type_match = true;

                            if (params.value("Type Name", "") == type_filter)
                                type_match = true;
                        }

                        if (!type_match)
                            continue;
                    }

                    if (!name_filter.empty())
                    {
                        bool name_match = false;

                        if (elem.contains("parameters") && elem["parameters"].is_object())
                        {
                            const json &params = elem["parameters"];

                            if (params.contains("Name") &&
                                params["Name"].is_string() &&
                                params["Name"].get<std::string>().find(name_filter) != std::string::npos)
                                name_match = true;

                            if (params.contains("Type Name") &&
                                params["Type Name"].is_string() &&
                                params["Type Name"].get<std::string>().find(name_filter) != std::string::npos)
                                name_match = true;

                            if (params.contains("Family and Type") &&
                                params["Family and Type"].is_string() &&
                                params["Family and Type"].get<std::string>().find(name_filter) != std::string::npos)
                                name_match = true;

                            if (params.contains("Family") &&
                                params["Family"].is_string() &&
                                params["Family"].get<std::string>().find(name_filter) != std::string::npos)
                                name_match = true;
                        }

                        if (!name_match)
                            continue;
                    }

                    json entry;
                    entry["bucket"] = group_name;
                    entry["uniqueId"] = elem.value("uniqueId", "");
                    entry["intId"] = elem.value("intId", 0);
                    entry["typeId"] = elem.value("typeId", "");
                    entry["hostId"] = elem.value("hostId", "");
                    entry["levelId"] = elem.value("levelId", "");
                    entry["category"] = elem.value("category", "");
                    entry["family"] = elem.value("family", "");
                    entry["type"] = elem.value("type", "");

                    if (elem.contains("parameters") && elem["parameters"].is_object())
                    {
                        const json &params = elem["parameters"];
                        entry["name"] = params.value("Name", "");
                        entry["typeName"] = params.value("Type Name", "");
                        entry["familyName"] = params.value("Family Name", "");
                        entry["category"] = entry.value("category", params.value("Category", ""));
                    }

                    results.push_back(entry);
                }
            }
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

        std::string clicked_id = req.value("globalId", "");
        int64_t node_index = req.value("nodeIndex", -1);

        if (clicked_id.empty() && node_index < 0)
        {
            response = R"({"error":"missing globalId or nodeIndex"})";
            return true;
        }

        std::string map_path = resolve_map_path(model);
        std::string model_name = model_name_from_map_path(model);
        std::string sem_path = resolve_semantic_path_from_model(model);

        dbg("Starting", "Debugging", true);
        dbg("GET_ELEMENT map_path:", map_path);
        dbg("GET_ELEMENT model_name:", model_name);
        dbg("GET_ELEMENT sem_path:", sem_path);
        dbg("GET_ELEMENT clicked_id:", clicked_id);

        bool map_ok = false;
        bool semantic_ok = false;
        bool geometry_ok = false;

        json nodes = json::array();
        json selected_element = json::object();
        json resolved_element = json::object();
        json properties = json::object();
        json traversal_path = json::array();

        std::string semantic_unique_id;

        json map;
        std::ifstream min(map_path);

        dbg("GET_ELEMENT map_open:", min.good() ? "true" : "false");

        if (min.good())
        {
            min >> map;
            min.close();

            map_ok = true;
            semantic_unique_id = resolve_unique_id_from_map(map, clicked_id, node_index);

            if (!semantic_unique_id.empty() && map.contains(semantic_unique_id))
            {
                geometry_ok = true;
                nodes = map[semantic_unique_id];
            }
        }

        dbg("GET_ELEMENT resolved_uniqueId:", semantic_unique_id);

        json sem;
        bool sem_loaded = load_json_mmap(sem_path, sem);

        dbg("GET_ELEMENT sem_load:", sem_loaded ? "true" : "false");

        if (sem_loaded && !semantic_unique_id.empty())
        {
            const json *found_elem = find_semantic_by_unique_id(sem, semantic_unique_id);

            if (found_elem)
            {
                semantic_ok = true;
                selected_element = *found_elem;
                resolved_element = *found_elem;

                traversal_path.push_back(resolved_element.value("uniqueId", ""));

                for (int depth = 0; depth < 32; ++depth)
                {
                    const json *next = find_next_semantic_context(sem, resolved_element);
                    if (!next)
                        break;

                    resolved_element = *next;
                    traversal_path.push_back(resolved_element.value("uniqueId", ""));
                }

                if (resolved_element.contains("parameters") && resolved_element["parameters"].is_object())
                    properties = resolved_element["parameters"];
            }
            else
            {
                dbg("GET_ELEMENT semantic_not_found:", semantic_unique_id);
            }
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"clicked_id", clicked_id},
                         {"nodeIndex", node_index},
                         {"map", map_ok},
                         {"semantic", semantic_ok},
                         {"geometry", geometry_ok},
                         {"semantic_uniqueId", semantic_unique_id},
                         {"node_indices", nodes},
                         {"selected_element", selected_element},
                         {"resolved_element", resolved_element},
                         {"element", resolved_element},
                         {"properties", properties},
                         {"traversal_path", traversal_path}})
                       .dump();

        return true;
    }

    static bool handle_action_validate_id(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "validate_id")
            return false;

        std::string clicked_id = req.value("globalId", "");
        int64_t node_index = req.value("nodeIndex", -1);

        if (clicked_id.empty() && node_index < 0)
        {
            response = R"({"error":"missing globalId or nodeIndex"})";
            return true;
        }

        std::string map_path = resolve_map_path(model);
        std::string sem_path = resolve_semantic_path_from_model(model);

        bool map_ok = false;
        bool semantic_ok = false;
        bool geometry_ok = false;

        json properties = json::object();
        json nodes = json::array();

        std::string semantic_unique_id;

        json map;
        std::ifstream min(map_path);

        if (min.good())
        {
            min >> map;
            min.close();

            map_ok = true;
            semantic_unique_id = resolve_unique_id_from_map(map, clicked_id, node_index);

            if (!semantic_unique_id.empty() && map.contains(semantic_unique_id))
            {
                geometry_ok = true;
                nodes = map[semantic_unique_id];
            }
        }

        json sem;
        if (load_json_mmap(sem_path, sem) && !semantic_unique_id.empty())
        {
            const json *found_elem = find_semantic_by_unique_id(sem, semantic_unique_id);

            if (found_elem)
            {
                semantic_ok = true;

                if (found_elem->contains("parameters") && (*found_elem)["parameters"].is_object())
                    properties = (*found_elem)["parameters"];
            }
        }

        response = json({{"plugin", "AIQuery"},
                         {"status", "ok"},
                         {"clicked_id", clicked_id},
                         {"nodeIndex", node_index},
                         {"map", map_ok},
                         {"semantic", semantic_ok},
                         {"geometry", geometry_ok},
                         {"semantic_uniqueId", semantic_unique_id},
                         {"properties", properties},
                         {"node_indices", nodes}})
                       .dump();

        return true;
    }

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

            if (handle_action_semantic_filter(req, model, response))
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