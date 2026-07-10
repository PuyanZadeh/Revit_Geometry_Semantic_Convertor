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
#include <unordered_map>
#include <unordered_set>
#include <functional>
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

    // Calls `visit(item)` for every semantic object anywhere in the tree,
    // across every bucket. The one place that knows the tree is shaped as
    // top-level buckets each holding an "items" array -- reverse traversal
    // below reuses this instead of re-implementing the walk that
    // find_semantic_by_unique_id (just below) already does for its own
    // early-exit point lookup.
    static void for_each_semantic_item(const json &sem, const std::function<void(const json &)> &visit)
    {
        if (!sem.is_object())
            return;

        for (const auto &group : sem.items())
        {
            const json &bucket = group.value();

            if (!bucket.is_object() || !bucket.contains("items") || !bucket["items"].is_array())
                continue;

            for (const auto &item : bucket["items"])
            {
                if (!item.is_object())
                    continue;

                visit(item);
            }
        }
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

    // ------------------------------------------------------------------
    // Generic explicit-relationship traversal (Step 2).
    //
    // A relationship field's value is a uniqueId reference to another
    // semantic object -- hostId, parentId, or any future field the
    // exporter adds that represents a real composition/ownership link
    // between model objects. This layer never knows what a relationship
    // "means" (host vs parent vs whatever comes later); it only follows
    // whatever field name the caller explicitly names, reusing the same
    // whole-tree find_semantic_by_unique_id lookup used elsewhere.
    //
    // Classification links (see classification_link_fields() below) are
    // deliberately kept out of this mechanism -- not handled by teaching
    // this layer field names, but by a guard at the request boundary in
    // handle_action_semantic_filter, which is the only place that should
    // be enforcing that distinction.
    // ------------------------------------------------------------------

    // Fields that identify a record's CLASSIFICATION (what kind/type of
    // thing it is) rather than its semantic context (what it's physically
    // connected to). typeId is the current example: instance -> type is
    // used for parameter inheritance (see resolve_field_with_fallback /
    // fallback_via_type below), never for graph traversal -- doing so would
    // let a query silently swap "related model objects" for "the type
    // record" it's classified under. Any future classification-only
    // relationship (as opposed to a real composition/ownership one like
    // hostId/parentId) belongs in this same set, not as a one-off check
    // scattered elsewhere.
    static const std::unordered_set<std::string> &classification_link_fields()
    {
        static const std::unordered_set<std::string> fields = {"typeId"};
        return fields;
    }

    // A relationship name is only valid if it's actually a field the
    // exporter produces -- determined from the loaded model's own data, not
    // a hardcoded list, so a genuinely new exporter field works
    // automatically while a typo ("hotsId") is never mistaken for a real
    // relationship (it will never appear as a key on any object).
    static bool relationship_exists_in_schema(const json &sem, const std::string &field)
    {
        bool found = false;

        for_each_semantic_item(sem, [&](const json &item)
                                {
            if (!found && item.contains(field))
                found = true; });

        return found;
    }

    // One traversal hop: follows `relationship` on `item` to the semantic
    // object it references, or nullptr if the field is absent or doesn't
    // resolve. Generic over the field name -- callers decide what's a valid
    // relationship to ask for; this function just follows it.
    static const json *traverse_relationship(const json &sem, const json &item, const std::string &relationship)
    {
        if (!item.is_object())
            return nullptr;

        auto it = item.find(relationship);
        if (it == item.end() || !it->is_string())
            return nullptr;

        return find_semantic_by_unique_id(sem, it->get<std::string>());
    }

    // Applies one traversal hop across a whole result set: each item's
    // connected object (via `relationship`) is collected, deduplicated by
    // uniqueId (first-seen order) so multiple starting items that share the
    // same connected object don't produce duplicates. Items that don't
    // resolve contribute nothing (there is no connected object to return).
    static json traverse_results(const json &sem, const json &items, const std::string &relationship)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            const json *related = traverse_relationship(sem, item, relationship);
            if (!related)
                continue;

            std::string key = related->value("uniqueId", "");
            if (!key.empty() && !seen.insert(key).second)
                continue;

            out.push_back(*related);
        }

        return out;
    }

    // Reverse hop: finds every semantic object anywhere in the tree whose
    // `relationship` field points AT `item`'s own uniqueId -- the mirror
    // image of traverse_relationship (which follows a reference FROM item).
    // Inherently one-to-many (many objects can host off of the same wall),
    // so this returns an array rather than a single object. Reuses
    // for_each_semantic_item instead of re-walking the tree itself.
    static json traverse_relationship_reverse(const json &sem, const json &item, const std::string &relationship)
    {
        json out = json::array();

        std::string target_id = item.value("uniqueId", "");
        if (target_id.empty())
            return out;

        for_each_semantic_item(sem, [&](const json &candidate)
                                {
            auto it = candidate.find(relationship);
            if (it != candidate.end() && it->is_string() && it->get<std::string>() == target_id)
                out.push_back(candidate); });

        return out;
    }

    // Applies one reverse hop across a whole result set: every object
    // referencing any of `items` via `relationship` is collected,
    // deduplicated by uniqueId (first-seen order) -- same dedup rule as the
    // forward direction's traverse_results.
    static json traverse_results_reverse(const json &sem, const json &items, const std::string &relationship)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            json matches = traverse_relationship_reverse(sem, item, relationship);

            for (const auto &match : matches)
            {
                std::string key = match.value("uniqueId", "");
                if (!key.empty() && !seen.insert(key).second)
                    continue;

                out.push_back(match);
            }
        }

        return out;
    }

    // One traversal chain step: a relationship field name plus which
    // direction to follow it in. A bare string in the request (see
    // to_traversal_steps) always means forward, so every chain that only
    // ever used bare strings behaves exactly as it did before reverse
    // traversal existed.
    struct TraversalStep
    {
        std::string field;
        bool reverse;
    };

    // Normalizes a "traverse" request value into an ordered list of steps.
    // Each entry is either a bare string (forward) or an object
    // {"field": "...", "direction": "forward"|"reverse"}. Mirrors
    // to_field_list's bare-value-or-array leniency for select/distinct.
    static std::vector<TraversalStep> to_traversal_steps(const json &v)
    {
        std::vector<TraversalStep> out;

        auto add_one = [&](const json &entry)
        {
            if (entry.is_string())
            {
                out.push_back({entry.get<std::string>(), false});
            }
            else if (entry.is_object())
            {
                std::string field = entry.value("field", "");
                std::string direction = entry.value("direction", "forward");
                if (!field.empty())
                    out.push_back({field, direction == "reverse"});
            }
        };

        if (v.is_string() || v.is_object())
            add_one(v);
        else if (v.is_array())
            for (const auto &entry : v)
                add_one(entry);

        return out;
    }

    // Applies an ordered chain of traversal hops (forward or reverse per
    // step), feeding each hop's output into the next. A one-entry chain is
    // a single hop; nothing about this function changes for longer chains
    // or mixed directions -- that's what "naturally supports longer
    // chains" means here.
    static json apply_traversal_chain(const json &sem, const json &items, const std::vector<TraversalStep> &chain)
    {
        json current = items;

        for (const auto &step : chain)
            current = step.reverse
                          ? traverse_results_reverse(sem, current, step.field)
                          : traverse_results(sem, current, step.field);

        return current;
    }

    const char *plugin_name()
    {
        return "AI Query Plugin";
    }

    const char *handle_ifc_aiquery(const std::string &input_json);

    // Dispatches an already-formed AST (op/match/filters, the same shape
    // ai_nl_to_ast produces) to the matching existing action. Shared by
    // handle_action_nl_query (ast comes from NLP translation) and
    // handle_action_structured_query (ast comes directly from the caller,
    // no translation involved) so there is exactly one place that maps an
    // AST onto the underlying engine actions.
    static bool dispatch_ast(const json &ast, const std::string &model, std::string &response)
    {
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

        if (op == "semantic_filter")
        {
            json fwd = {
                {"action", "semantic_filter"},
                {"model", model},
                {"match", ast.value("match", "all")},
                {"filters", ast.value("filters", json::array())}};

            // Pass the result-shaping fields through untouched when present.
            // These are optional on every caller of dispatch_ast (NLP or
            // structured_query); absent here means absent on the forwarded
            // semantic_filter call, which is what preserves today's behavior.
            if (ast.contains("traverse")) fwd["traverse"] = ast["traverse"];
            if (ast.contains("select")) fwd["select"] = ast["select"];
            if (ast.contains("distinct")) fwd["distinct"] = ast["distinct"];
            if (ast.contains("group_by")) fwd["group_by"] = ast["group_by"];
            if (ast.contains("sort")) fwd["sort"] = ast["sort"];
            if (ast.contains("limit")) fwd["limit"] = ast["limit"];

            response = handle_ifc_aiquery(fwd.dump());

            // TEMP-DEBUG(nlp-validation): remove this block, and the matching frontend
            // panel in SemanticFilterPanel.tsx, once the NLP layer has been validated.
            try
            {
                json resp_json = json::parse(response);
                resp_json["debug_nlp_translation"] = ast;
                response = resp_json.dump();
            }
            catch (...)
            {
            }
            // END TEMP-DEBUG(nlp-validation)

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

        json ast = ai_nl_to_ast(nl, model);
        return dispatch_ast(ast, model, response);
    }

    // Submits an AST (op/match/filters) directly, bypassing NLP translation
    // entirely -- goes straight to dispatch_ast, the same dispatcher
    // handle_action_nl_query uses. Intended for direct testing, APIs,
    // future integrations (e.g. a Revit plugin), regression tests, and
    // benchmarking of the semantic filtering engine independent of the NLP
    // layer. This action itself is permanent; only the temporary frontend
    // debug UI that currently exercises it is meant to go away later.
    static bool handle_action_structured_query(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "structured_query")
            return false;

        json ast = req.value("ast", json::object());
        return dispatch_ast(ast, model, response);
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

    // Revit-like parameter inheritance: a fallback step takes the semantic
    // root and the ORIGINAL item and returns a related record to also check
    // (or nullptr if this step doesn't apply). Registered in priority order
    // in parameter_fallback_chain() below. Adding a new inheritance path
    // (family, host, ...) means writing one more step function and adding it
    // to that list -- nothing that resolves a field ever needs to change.
    using FallbackStep = const json *(*)(const json &sem, const json &item);

    // instance -> type: follows the instance's typeId to its type record
    // (the "types" bucket in semantic.json), reusing the existing generic
    // whole-tree uniqueId lookup already used by the traversal-chain feature.
    static const json *fallback_via_type(const json &sem, const json &item)
    {
        // Same lookup as traverse_relationship's "typeId" case; this is the
        // one sanctioned use of that classification link (see
        // classification_link_fields()) -- parameter inheritance, not graph
        // traversal.
        return traverse_relationship(sem, item, "typeId");
    }

    static const std::vector<FallbackStep> &parameter_fallback_chain()
    {
        static const std::vector<FallbackStep> chain = {fallback_via_type};
        return chain;
    }

    // resolve_field and resolve_path (defined below) share this exact
    // signature, which is what lets resolve_with_fallback below accept
    // either one as a plain function pointer -- no template needed (and
    // templates aren't allowed inside this file's extern "C" block anyway).
    using FieldResolver = const json *(*)(const json &item, const std::string &field_or_path);

    // Resolves `field_or_path` on `item` using `base` (resolve_field or
    // resolve_path); if the item itself doesn't have it, tries the same
    // lookup on each fallback candidate in turn. This is the ONLY place that
    // knows about parameter inheritance -- resolve_field/resolve_path
    // themselves are unchanged and are what actually gets called at every
    // hop, so there is exactly one implementation of "how a value is read
    // off one record."
    static const json *resolve_with_fallback(const json &sem, const json &item, const std::string &field_or_path, FieldResolver base)
    {
        const json *direct = base(item, field_or_path);
        if (direct)
            return direct;

        for (auto step : parameter_fallback_chain())
        {
            const json *related = step(sem, item);
            if (!related)
                continue;

            const json *found = base(*related, field_or_path);
            if (found)
                return found;
        }

        return nullptr;
    }

    static const json *resolve_field_with_fallback(const json &sem, const json &item, const std::string &field)
    {
        return resolve_with_fallback(sem, item, field, resolve_field);
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
    static bool eval_filter_clause(const json &sem, const json &item, const json &clause)
    {
        if (!clause.is_object())
            return false;

        std::string field = clause.value("field", "");
        std::string op = clause.value("op", "eq");

        if (field.empty())
            return false;

        const json *actual = resolve_field_with_fallback(sem, item, field);

        if (op == "exists")
            return actual != nullptr && !actual->is_null();

        if (op == "not_exists")
            return actual == nullptr || actual->is_null();

        // Every other operator requires an actual, non-null value to compare
        // against. A missing field never satisfies eq/neq/contains/gt/gte/lt/lte;
        // callers that need to distinguish "absent" should use "exists"/"not_exists".
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
    static bool eval_filters(const json &sem, const json &item, const json &filters, const std::string &match)
    {
        if (!filters.is_array() || filters.empty())
            return true;

        bool require_all = (match != "any");

        for (const auto &clause : filters)
        {
            bool clause_result = eval_filter_clause(sem, item, clause);

            if (require_all && !clause_result)
                return false;

            if (!require_all && clause_result)
                return true;
        }

        return require_all;
    }

    // ------------------------------------------------------------------
    // Generic result shaping: select / distinct / group_by / sort / limit
    // (Step 1 extension). Everything here is field-name-agnostic, same as
    // the filtering engine above -- no field or category is ever special-
    // cased. These stages run strictly after filtering and never change
    // filtering behavior; when none of them are requested, semantic_filter
    // produces exactly the response it always has.
    // ------------------------------------------------------------------

    // Like resolve_field, but also understands dotted paths ("parameters.Family",
    // "refs.fromRoomId") for arbitrary nesting depth. A path with no dot defers
    // to resolve_field unchanged, so bare field names keep the same top-level-
    // then-parameters fallback that filters already rely on.
    static const json *resolve_path(const json &item, const std::string &path)
    {
        if (path.find('.') == std::string::npos)
            return resolve_field(item, path);

        if (!item.is_object())
            return nullptr;

        const json *cur = &item;
        size_t start = 0;

        while (true)
        {
            size_t dot = path.find('.', start);
            std::string segment = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);

            if (!cur->is_object())
                return nullptr;

            auto it = cur->find(segment);
            if (it == cur->end())
                return nullptr;

            cur = &(*it);

            if (dot == std::string::npos)
                return cur;

            start = dot + 1;
        }
    }

    // Same fallback mechanism as resolve_field_with_fallback (defined above,
    // right after resolve_field), reused here for resolve_path so select/
    // distinct/group_by/sort see the identical instance -> type (-> ...)
    // inheritance that filtering does.
    static const json *resolve_path_with_fallback(const json &sem, const json &item, const std::string &path)
    {
        return resolve_with_fallback(sem, item, path, resolve_path);
    }

    // Writes `value` into `out` at a (possibly dotted) path, creating
    // intermediate objects as needed. Used to reconstruct the original
    // nesting shape for select/distinct output.
    static void set_path(json &out, const std::string &path, const json &value)
    {
        json *cur = &out;
        size_t start = 0;

        while (true)
        {
            size_t dot = path.find('.', start);
            std::string segment = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);

            if (dot == std::string::npos)
            {
                (*cur)[segment] = value;
                return;
            }

            if (!cur->contains(segment) || !(*cur)[segment].is_object())
                (*cur)[segment] = json::object();

            cur = &(*cur)[segment];
            start = dot + 1;
        }
    }

    // Normalizes a "select"/"distinct" request field into a list of path
    // strings: accepts either a single string or an array of strings.
    static std::vector<std::string> to_field_list(const json &v)
    {
        std::vector<std::string> out;

        if (v.is_string())
        {
            out.push_back(v.get<std::string>());
        }
        else if (v.is_array())
        {
            for (const auto &f : v)
                if (f.is_string())
                    out.push_back(f.get<std::string>());
        }

        return out;
    }

    // Projects one item down to just the requested paths, reconstructed with
    // their original nesting. Missing paths come back as null so the output
    // shape is predictable regardless of which items happen to have them.
    static json project_fields(const json &sem, const json &item, const std::vector<std::string> &fields)
    {
        json out = json::object();

        for (const auto &f : fields)
        {
            const json *v = resolve_path_with_fallback(sem, item, f);
            set_path(out, f, v ? *v : json(nullptr));
        }

        return out;
    }

    // Generic comparator for sorting: numeric compare if both sides parse as
    // numbers (same coercion rule gt/gte/lt/lte already use), else string
    // compare. Never branches on field name.
    static int compare_json_values(const json &a, const json &b)
    {
        double da, db;

        if (json_value_to_number(a, da) && json_value_to_number(b, db))
        {
            if (da < db)
                return -1;
            if (da > db)
                return 1;
            return 0;
        }

        std::string sa = json_to_compare_string(a);
        std::string sb = json_to_compare_string(b);

        if (sa < sb)
            return -1;
        if (sa > sb)
            return 1;
        return 0;
    }

    // Sorts `arr` in place by `field` (a resolve_path path). Entries missing
    // the field always sort last, regardless of direction, for determinism.
    static void sort_results(const json &sem, json &arr, const std::string &field, bool descending)
    {
        if (!arr.is_array() || field.empty())
            return;

        std::vector<json> items(arr.begin(), arr.end());

        std::stable_sort(items.begin(), items.end(), [&](const json &lhs, const json &rhs)
                          {
            const json *lv = resolve_path_with_fallback(sem, lhs, field);
            const json *rv = resolve_path_with_fallback(sem, rhs, field);

            bool l_missing = (!lv || lv->is_null());
            bool r_missing = (!rv || rv->is_null());

            if (l_missing != r_missing)
                return !l_missing;

            if (l_missing && r_missing)
                return false;

            int c = compare_json_values(*lv, *rv);
            if (descending)
                c = -c;

            return c < 0; });

        arr = json(items);
    }

    // group_by: collapses `items` into one {"value","count"} entry per
    // distinct value of `field`, in first-seen order.
    static json group_by_results(const json &sem, const json &items, const std::string &field)
    {
        std::vector<std::string> order;
        std::unordered_map<std::string, int> counts;
        std::unordered_map<std::string, json> values;

        for (const auto &item : items)
        {
            const json *v = resolve_path_with_fallback(sem, item, field);
            json key_value = v ? *v : json(nullptr);
            std::string key = json_to_compare_string(key_value);

            if (counts.find(key) == counts.end())
            {
                order.push_back(key);
                values[key] = key_value;
            }

            counts[key]++;
        }

        json out = json::array();
        for (const auto &key : order)
            out.push_back({{"value", values[key]}, {"count", counts[key]}});

        return out;
    }

    // distinct: collapses `items` into one entry per unique combination of
    // `fields`, in first-seen order, shaped like select's output.
    static json distinct_results(const json &sem, const json &items, const std::vector<std::string> &fields)
    {
        std::unordered_map<std::string, bool> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            std::string key;
            for (const auto &f : fields)
            {
                const json *v = resolve_path_with_fallback(sem, item, f);
                key += json_to_compare_string(v ? *v : json(nullptr));
                key += '\x1f';
            }

            if (seen.find(key) != seen.end())
                continue;

            seen[key] = true;
            out.push_back(project_fields(sem, item, fields));
        }

        return out;
    }

    // select: projects every item down to just `fields`, one output entry
    // per matched item (not deduplicated).
    static json select_results(const json &sem, const json &items, const std::vector<std::string> &fields)
    {
        json out = json::array();

        for (const auto &item : items)
            out.push_back(project_fields(sem, item, fields));

        return out;
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

                    if (eval_filters(sem, item, filters, match))
                        results.push_back(item);
                }
            }
        }

        std::vector<TraversalStep> traverse_chain = to_traversal_steps(req.value("traverse", json::array()));
        bool has_traverse = !traverse_chain.empty();

        if (has_traverse)
        {
            for (const auto &step : traverse_chain)
            {
                if (classification_link_fields().count(step.field))
                {
                    response = json({{"error", "\"" + step.field + "\" is a classification link, not a traversal relationship"}}).dump();
                    return true;
                }

                if (!relationship_exists_in_schema(sem, step.field))
                {
                    response = json({{"error", "Unknown traversal relationship: \"" + step.field + "\""}}).dump();
                    return true;
                }
            }

            results = apply_traversal_chain(sem, results, traverse_chain);
        }

        bool has_group_by = req.contains("group_by") && req["group_by"].is_string() && !req["group_by"].get<std::string>().empty();
        bool has_distinct = req.contains("distinct") && !to_field_list(req["distinct"]).empty();
        bool has_select = req.contains("select") && !to_field_list(req["select"]).empty();

        int shape_count = (has_group_by ? 1 : 0) + (has_distinct ? 1 : 0) + (has_select ? 1 : 0);
        if (shape_count > 1)
        {
            response = json({{"error", "group_by, distinct, and select are mutually exclusive"}}).dump();
            return true;
        }

        json shaped = results;

        if (has_group_by)
            shaped = group_by_results(sem, results, req["group_by"].get<std::string>());
        else if (has_distinct)
            shaped = distinct_results(sem, results, to_field_list(req["distinct"]));
        else if (has_select)
            shaped = select_results(sem, results, to_field_list(req["select"]));

        bool has_sort = req.contains("sort") && req["sort"].is_object();
        if (has_sort)
        {
            std::string sort_field = req["sort"].value("field", "");
            std::string sort_order = req["sort"].value("order", "asc");
            sort_results(sem, shaped, sort_field, sort_order == "desc");
        }

        size_t shaped_count = shaped.size();

        bool has_limit = req.contains("limit") && req["limit"].is_number_integer();
        if (has_limit)
        {
            int64_t lim = req["limit"].get<int64_t>();
            if (lim < 0)
                lim = 0;

            if (static_cast<size_t>(lim) < shaped.size())
            {
                json truncated = json::array();
                for (int64_t i = 0; i < lim; ++i)
                    truncated.push_back(shaped[static_cast<size_t>(i)]);
                shaped = truncated;
            }
        }

        json resp = {{"plugin", "AIQuery"},
                     {"status", "ok"},
                     {"action", "semantic_filter"},
                     {"match", match},
                     {"filters", filters},
                     {"count", shaped_count},
                     {"results", shaped}};

        if (has_traverse)
            resp["traverse"] = req["traverse"];
        if (has_group_by)
            resp["group_by"] = req["group_by"];
        if (has_distinct)
            resp["distinct"] = req["distinct"];
        if (has_select)
            resp["select"] = req["select"];
        if (has_sort)
            resp["sort"] = req["sort"];
        if (has_limit)
            resp["limit"] = req["limit"];

        response = resp.dump();

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

            if (handle_action_structured_query(req, model, response))
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