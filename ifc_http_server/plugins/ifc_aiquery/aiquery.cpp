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

    // Step 12 optimization #1: uniqueId -> object index.
    //
    // find_semantic_by_unique_id used to do a full linear scan over every
    // bucket/item for every single lookup -- the single most-called
    // primitive in the engine (every forward traversal hop, and every
    // type-inheritance fallback triggered whenever a filter/select/sort/
    // group_by/aggregate/constraint field isn't present directly on the
    // instance). Building this index once per request turns that into an
    // O(1) lookup, without changing any lookup's observable result.
    //
    // Built explicitly per request (build_unique_id_index, called once by
    // each action handler right after load_json_mmap) and threaded through
    // this lightweight context -- never cached in static/global state. The
    // plugin has no state across requests (dlopen'd fresh per request), so
    // there'd be nowhere safe to hide it even if that seemed convenient.
    // Step 12 optimization #2: reverse-relationship index, keyed by field
    // name (a bare field like "hostId" or a dotted path like
    // "refs.fromRoomId" -- resolve_relationship_value already walks dotted
    // paths, so the cache key is just whatever string the caller asked to
    // reverse-traverse on, verbatim). Built lazily -- only the first time a
    // given field is actually reverse-traversed in this request -- and
    // reused for every subsequent reverse hop on that same field (including
    // every depth of a recursive reverse walk). Like id_index, this lives in
    // the per-request SemanticContext, never in static/global/thread-local
    // state; the reference member itself is mutable so the cache can be
    // filled in lazily even though callers only ever hold a const
    // SemanticContext&.
    // Step 12 optimization #2: reverse-relationship index, keyed by field
    // name (a bare field like "hostId" or a dotted path like
    // "refs.fromRoomId" -- resolve_relationship_value already walks dotted
    // paths, so the cache key is just whatever string the caller asked to
    // reverse-traverse on, verbatim). Built lazily -- only the first time a
    // given field is actually reverse-traversed in this request -- and
    // reused for every subsequent reverse hop on that same field (including
    // every depth of a recursive reverse walk). Like id_index, this lives in
    // the per-request SemanticContext, never in static/global/thread-local
    // state; the reference member itself is mutable so the cache can be
    // filled in lazily even though callers only ever hold a const
    // SemanticContext&.
    struct SemanticContext
    {
        const json &tree;
        const std::unordered_map<std::string, const json *> &id_index;
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const json *>>> &reverse_index_cache;
    };

    // Builds the index in one O(N) pass, reusing for_each_semantic_item so
    // the walk order is identical to find_semantic_by_unique_id's original
    // scan (buckets in object-iteration order, items in array order). On a
    // duplicate uniqueId the FIRST occurrence encountered wins here too --
    // preserving the old linear scan's first-match behavior exactly.
    // Duplicates are logged rather than treated as errors, so Step 12
    // testing can report how many (if any) exist in a given model.
    static std::unordered_map<std::string, const json *> build_unique_id_index(const json &sem)
    {
        std::unordered_map<std::string, const json *> index;
        size_t duplicate_count = 0;

        for_each_semantic_item(sem, [&](const json &item)
                                {
            std::string id = item.value("uniqueId", "");
            if (id.empty())
                return;

            if (index.find(id) != index.end())
            {
                duplicate_count++;
                return;
            }

            index[id] = &item; });

        if (duplicate_count > 0)
            dbg("build_unique_id_index duplicate uniqueId count:", std::to_string(duplicate_count));

        return index;
    }

    static const json *find_semantic_by_unique_id(const SemanticContext &ctx, const std::string &uniqueId)
    {
        auto it = ctx.id_index.find(uniqueId);
        return it != ctx.id_index.end() ? it->second : nullptr;
    }

    static const json *find_next_semantic_context(const SemanticContext &ctx, const json &element)
    {
        if (!element.is_object())
            return nullptr;

        if (element.contains("hostId") && element["hostId"].is_string())
            return find_semantic_by_unique_id(ctx, element["hostId"].get<std::string>());

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
    //
    // Note this set is about classification vs. semantic-context, a
    // different axis from the hierarchical/associative distinction below --
    // hostId/parentId are hierarchical (one object physically contains or
    // owns another); refs.fromRoomId/refs.toRoomId are associative (a Door
    // references two Rooms it connects; neither owns the other). Both kinds
    // are equally valid traversal edges to this engine -- it stays
    // field-name-agnostic either way -- this is a documentation distinction
    // for callers, not a behavioral one.
    static const std::unordered_set<std::string> &classification_link_fields()
    {
        static const std::unordered_set<std::string> fields = {"typeId"};
        return fields;
    }

    // Resolves a relationship field's value on `item`: a bare name is a
    // direct top-level lookup (unchanged from before dotted-path support
    // existed); a dotted path ("refs.fromRoomId") walks nested objects.
    // This mirrors resolve_path's dotted-walk (used by select/group_by/
    // sort), duplicated locally rather than calling it directly, since
    // resolve_path is defined much later in this file and reordering large
    // sections isn't worth the risk for a small walk like this.
    static const json *resolve_relationship_value(const json &item, const std::string &field)
    {
        if (!item.is_object())
            return nullptr;

        if (field.find('.') == std::string::npos)
        {
            auto it = item.find(field);
            return it != item.end() ? &(*it) : nullptr;
        }

        const json *cur = &item;
        size_t start = 0;

        while (true)
        {
            size_t dot = field.find('.', start);
            std::string segment = field.substr(start, dot == std::string::npos ? std::string::npos : dot - start);

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

    // A relationship name is only valid if it's actually a field the
    // exporter produces -- determined from the loaded model's own data, not
    // a hardcoded list, so a genuinely new exporter field works
    // automatically while a typo ("hotsId") is never mistaken for a real
    // relationship (it will never appear as a key, or nested path, on any
    // object).
    static bool relationship_exists_in_schema(const json &sem, const std::string &field)
    {
        bool found = false;

        for_each_semantic_item(sem, [&](const json &item)
                                {
            if (!found && resolve_relationship_value(item, field) != nullptr)
                found = true; });

        return found;
    }

    // One traversal hop: follows `relationship` on `item` to the semantic
    // object it references, or nullptr if the field is absent or doesn't
    // resolve. Generic over the field name -- callers decide what's a valid
    // relationship to ask for; this function just follows it.
    static const json *traverse_relationship(const SemanticContext &ctx, const json &item, const std::string &relationship)
    {
        const json *value = resolve_relationship_value(item, relationship);
        if (!value || !value->is_string())
            return nullptr;

        return find_semantic_by_unique_id(ctx, value->get<std::string>());
    }

    // Applies one traversal hop across a whole result set: each item's
    // connected object (via `relationship`) is collected, deduplicated by
    // uniqueId (first-seen order) so multiple starting items that share the
    // same connected object don't produce duplicates. Items that don't
    // resolve contribute nothing (there is no connected object to return).
    static json traverse_results(const SemanticContext &ctx, const json &items, const std::string &relationship)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            const json *related = traverse_relationship(ctx, item, relationship);
            if (!related)
                continue;

            std::string key = related->value("uniqueId", "");
            if (!key.empty() && !seen.insert(key).second)
                continue;

            out.push_back(*related);
        }

        return out;
    }

    // Builds (or returns the already-built) reverse index for one
    // relationship field: target uniqueId -> every object whose
    // `relationship` field points at it. `relationship` is passed straight
    // to resolve_relationship_value unchanged, so a dotted path like
    // "refs.fromRoomId" is indexed exactly as correctly as a bare field like
    // "hostId" -- the field-name string is just an opaque cache key here;
    // this function never interprets it beyond that.
    //
    // One O(N) pass over the whole model builds the WHOLE index for this
    // field in one go (rather than one scan per starting item), bucketing
    // each candidate under whichever target it references, in tree-walk
    // order -- so for any given target, its bucket's order is identical to
    // what a dedicated linear scan for just that target would have produced
    // (same fixed walk order either way). Cached in the per-request context
    // so a second reverse hop on the same field (including every depth of a
    // recursive reverse walk) never rebuilds it.
    static const std::unordered_map<std::string, std::vector<const json *>> &get_or_build_reverse_index(const SemanticContext &ctx, const std::string &relationship)
    {
        auto cached = ctx.reverse_index_cache.find(relationship);
        if (cached != ctx.reverse_index_cache.end())
            return cached->second;

        std::unordered_map<std::string, std::vector<const json *>> index;

        for_each_semantic_item(ctx.tree, [&](const json &candidate)
                                {
            const json *value = resolve_relationship_value(candidate, relationship);
            if (value && value->is_string())
                index[value->get<std::string>()].push_back(&candidate); });

        auto inserted = ctx.reverse_index_cache.emplace(relationship, std::move(index));
        return inserted.first->second;
    }

    // Reverse hop: finds every semantic object anywhere in the tree whose
    // `relationship` field points AT `item`'s own uniqueId -- the mirror
    // image of traverse_relationship (which follows a reference FROM item).
    // Inherently one-to-many (many objects can host off of the same wall),
    // so this returns an array rather than a single object. Looks the target
    // up in the lazily-built reverse index instead of re-scanning the tree.
    static json traverse_relationship_reverse(const SemanticContext &ctx, const json &item, const std::string &relationship)
    {
        json out = json::array();

        std::string target_id = item.value("uniqueId", "");
        if (target_id.empty())
            return out;

        const auto &reverse_index = get_or_build_reverse_index(ctx, relationship);
        auto matches = reverse_index.find(target_id);
        if (matches == reverse_index.end())
            return out;

        for (const json *candidate : matches->second)
            out.push_back(*candidate);

        return out;
    }

    // Applies one reverse hop across a whole result set: every object
    // referencing any of `items` via `relationship` is collected,
    // deduplicated by uniqueId (first-seen order) -- same dedup rule as the
    // forward direction's traverse_results.
    static json traverse_results_reverse(const SemanticContext &ctx, const json &items, const std::string &relationship)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            json matches = traverse_relationship_reverse(ctx, item, relationship);

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

    // Validates every step in a flat chain against the same rules used
    // everywhere traversal is requested (classification links rejected,
    // unknown relationships rejected). Extracted from what was previously
    // an inline loop in handle_action_semantic_filter's "traverse" handling
    // so aggregate's graph-based group_by (Step 6) can reuse the identical
    // checks instead of re-deriving them; behavior for "traverse" itself is
    // unchanged, just relocated. Returns the first error found, or an empty
    // string if the whole chain is valid.
    static std::string validate_traversal_steps(const json &sem, const std::vector<TraversalStep> &chain)
    {
        for (const auto &step : chain)
        {
            if (classification_link_fields().count(step.field))
                return "\"" + step.field + "\" is a classification link, not a traversal relationship";

            if (!relationship_exists_in_schema(sem, step.field))
                return "Unknown traversal relationship: \"" + step.field + "\"";
        }

        return "";
    }

    // Applies an ordered chain of traversal hops (forward or reverse per
    // step), feeding each hop's output into the next. A one-entry chain is
    // a single hop; nothing about this function changes for longer chains
    // or mixed directions -- that's what "naturally supports longer
    // chains" means here.
    static json apply_traversal_chain(const SemanticContext &ctx, const json &items, const std::vector<TraversalStep> &chain)
    {
        json current = items;

        for (const auto &step : chain)
            current = step.reverse
                          ? traverse_results_reverse(ctx, current, step.field)
                          : traverse_results(ctx, current, step.field);

        return current;
    }

    // ------------------------------------------------------------------
    // Step 4: relationship path preservation.
    //
    // Deliberately NOT built by modifying traverse_results/traverse_results_
    // reverse/apply_traversal_chain above -- those stay exactly as validated
    // in Steps 2-3. This is a separate, parallel orchestration that reuses
    // the same per-item resolution primitives (traverse_relationship /
    // traverse_relationship_reverse) but additionally records, per hop,
    // which source object reached which target object. Edges store only
    // uniqueId references -- the actual objects still live exactly once, in
    // the (unchanged) results array, never copied into the path data.
    // ------------------------------------------------------------------

    // One hop, with edge recording. Same dedup rule as traverse_results/
    // traverse_results_reverse (each distinct target appears once in the
    // returned set), but every (from, to) pair actually followed is
    // appended to `edges_out` regardless of whether its target was already
    // seen -- two different origins reaching the same target are two
    // distinct edges against one shared result object.
    static json traverse_results_with_edges(const SemanticContext &ctx, const json &items, const TraversalStep &step, json &edges_out)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            std::string from_id = item.value("uniqueId", "");

            if (step.reverse)
            {
                json matches = traverse_relationship_reverse(ctx, item, step.field);

                for (const auto &match : matches)
                {
                    std::string to_id = match.value("uniqueId", "");
                    edges_out.push_back({{"from", from_id}, {"to", to_id}});

                    if (!to_id.empty() && !seen.insert(to_id).second)
                        continue;

                    out.push_back(match);
                }
            }
            else
            {
                const json *related = traverse_relationship(ctx, item, step.field);
                if (!related)
                    continue;

                std::string to_id = related->value("uniqueId", "");
                edges_out.push_back({{"from", from_id}, {"to", to_id}});

                if (!to_id.empty() && !seen.insert(to_id).second)
                    continue;

                out.push_back(*related);
            }
        }

        return out;
    }

    // Applies the traversal chain like apply_traversal_chain, but also
    // builds `paths_out`: one entry per hop, in chain order, each holding
    // that hop's relationship/direction plus the edges it followed. A
    // longer chain just means more entries here -- nothing about this
    // function's shape changes for additional hops.
    static json apply_traversal_chain_with_paths(const SemanticContext &ctx, const json &items, const std::vector<TraversalStep> &chain, json &paths_out)
    {
        json current = items;

        for (const auto &step : chain)
        {
            json edges = json::array();
            current = traverse_results_with_edges(ctx, current, step, edges);

            paths_out.push_back({{"relationship", step.field},
                                  {"direction", step.reverse ? "reverse" : "forward"},
                                  {"edges", edges}});
        }

        return current;
    }

    // ------------------------------------------------------------------
    // Step 5: branching traversal.
    //
    // Deliberately a separate request key ("traverse_tree") and a separate
    // set of functions from everything above -- traverse_results/
    // traverse_results_reverse/apply_traversal_chain(_with_paths) and the
    // "traverse" request key are untouched, so every existing linear query
    // behaves exactly as before. A linear chain is the one-child special
    // case of this tree grammar; branching is what happens when a node has
    // more than one child. Reuses traverse_relationship/
    // traverse_relationship_reverse -- the same per-item resolution the
    // linear engine uses -- so there is still exactly one implementation of
    // "how a single hop is resolved."
    // ------------------------------------------------------------------

    // One node in a traversal tree: a hop (field/direction) plus what
    // happens after it. `then` empty -> leaf, this hop's output joins the
    // final union. `then` with one entry -> continue linearly. `then` with
    // more than one entry -> branch, each applied independently to this
    // hop's output.
    struct TraversalNode
    {
        std::string field;
        bool reverse;
        std::vector<TraversalNode> then;
    };

    static TraversalNode parse_traversal_node(const json &v)
    {
        TraversalNode node;
        node.field = v.value("field", "");
        node.reverse = v.value("direction", "forward") == "reverse";

        if (v.contains("then"))
        {
            const json &then = v["then"];

            if (then.is_array())
                for (const auto &child : then)
                    node.then.push_back(parse_traversal_node(child));
            else if (then.is_object())
                node.then.push_back(parse_traversal_node(then));
        }

        return node;
    }

    // A "forest" is the list of root nodes -- traverse_tree's top-level
    // value follows the same single-object-or-array grammar as `then`, so
    // root-level branching and nested branching share one shape.
    static std::vector<TraversalNode> parse_traversal_forest(const json &v)
    {
        std::vector<TraversalNode> roots;

        if (v.is_array())
            for (const auto &node : v)
                roots.push_back(parse_traversal_node(node));
        else if (v.is_object())
            roots.push_back(parse_traversal_node(v));

        return roots;
    }

    // Validates every field anywhere in the tree against the same rules
    // "traverse" already enforces (classification links rejected, unknown
    // relationships rejected) -- reuses classification_link_fields() and
    // relationship_exists_in_schema() rather than re-deriving the checks.
    // Returns the first error found, or an empty string if the whole tree
    // is valid.
    static std::string validate_traversal_forest(const json &sem, const std::vector<TraversalNode> &nodes)
    {
        for (const auto &node : nodes)
        {
            if (classification_link_fields().count(node.field))
                return "\"" + node.field + "\" is a classification link, not a traversal relationship";

            if (!relationship_exists_in_schema(sem, node.field))
                return "Unknown traversal relationship: \"" + node.field + "\"";

            std::string child_error = validate_traversal_forest(sem, node.then);
            if (!child_error.empty())
                return child_error;
        }

        return "";
    }

    // Recursively applies one node to `items`: records this hop's edges
    // (regardless of depth), then either contributes its deduplicated
    // output to the overall union (leaf) or recurses into each child
    // (branch), passing this hop's output as their input. `path_node_out`
    // mirrors the tree shape so branches remain distinguishable in the
    // response -- edges plus a nested "then" for whatever this node
    // branched into, omitted for a leaf.
    static void apply_traversal_node(const SemanticContext &ctx, const json &items, const TraversalNode &node,
                                      json &union_out, std::unordered_set<std::string> &union_seen,
                                      json &path_node_out)
    {
        json edges = json::array();
        json hop_out = json::array();
        std::unordered_set<std::string> hop_seen;

        for (const auto &item : items)
        {
            std::string from_id = item.value("uniqueId", "");

            if (node.reverse)
            {
                json matches = traverse_relationship_reverse(ctx, item, node.field);

                for (const auto &match : matches)
                {
                    std::string to_id = match.value("uniqueId", "");
                    edges.push_back({{"from", from_id}, {"to", to_id}});

                    if (to_id.empty() || hop_seen.insert(to_id).second)
                        hop_out.push_back(match);
                }
            }
            else
            {
                const json *related = traverse_relationship(ctx, item, node.field);
                if (!related)
                    continue;

                std::string to_id = related->value("uniqueId", "");
                edges.push_back({{"from", from_id}, {"to", to_id}});

                if (to_id.empty() || hop_seen.insert(to_id).second)
                    hop_out.push_back(*related);
            }
        }

        path_node_out["relationship"] = node.field;
        path_node_out["direction"] = node.reverse ? "reverse" : "forward";
        path_node_out["edges"] = edges;

        if (node.then.empty())
        {
            for (const auto &obj : hop_out)
            {
                std::string id = obj.value("uniqueId", "");
                if (id.empty() || union_seen.insert(id).second)
                    union_out.push_back(obj);
            }

            return;
        }

        json children = json::array();

        for (const auto &child : node.then)
        {
            json child_path = json::object();
            apply_traversal_node(ctx, hop_out, child, union_out, union_seen, child_path);
            children.push_back(child_path);
        }

        path_node_out["then"] = children;
    }

    // Applies a forest of root nodes (each independent, from the same
    // starting items) and returns the union of every leaf across the whole
    // tree, plus the tree-shaped path description in `paths_out`.
    static json apply_traversal_forest(const SemanticContext &ctx, const json &items, const std::vector<TraversalNode> &roots, json &paths_out)
    {
        json union_out = json::array();
        std::unordered_set<std::string> union_seen;

        for (const auto &root : roots)
        {
            json root_path = json::object();
            apply_traversal_node(ctx, items, root, union_out, union_seen, root_path);
            paths_out.push_back(root_path);
        }

        return union_out;
    }

    // ------------------------------------------------------------------
    // Step 11: recursive traversal.
    //
    // Repeatedly follows ONE relationship (forward or reverse) starting
    // from `items`, level by level, until max_depth is reached, a level
    // finds no new objects, or the hard safety cap below is hit regardless
    // of max_depth. Reuses traverse_relationship/traverse_relationship_
    // reverse -- the exact same per-item resolvers "traverse" already
    // uses -- for every hop; nothing about how a single hop resolves is
    // reimplemented here. Does not modify the filtering, aggregation,
    // constraint-evaluation, or set-operation engines.
    // ------------------------------------------------------------------

    // Absolute ceiling on recursion depth, applied even when the caller
    // omits max_depth (or supplies one larger than this). The visited set
    // already makes cycles converge naturally (a level that only reaches
    // already-seen objects finds nothing new and stops); this is purely a
    // safety net bounding worst-case work regardless.
    static const int64_t RECURSIVE_TRAVERSAL_HARD_CAP = 1000;

    // Result is every newly reached object across every depth level (not
    // just the final frontier) -- a depth-1 discovery is included in the
    // output even if the recursion continues on to depth 2, 3, etc. The
    // starting objects themselves are never included, only what's newly
    // reached from them, matching how "traverse"/"traverse_tree" already
    // never include the starting objects either.
    static json apply_recursive_traversal(const SemanticContext &ctx, const json &items, const std::string &field, bool reverse, int64_t max_depth, json &paths_out)
    {
        // max_depth < 0 means "omitted" (see call site) -- an explicit 0
        // means zero hops, so the loop below runs zero times.
        int64_t effective_max_depth = (max_depth >= 0 && max_depth < RECURSIVE_TRAVERSAL_HARD_CAP)
                                           ? max_depth
                                           : RECURSIVE_TRAVERSAL_HARD_CAP;

        // "visited" tracks objects discovered during THIS walk (depth >= 1),
        // not the starting items -- seeding it with the starting set would
        // wrongly block legitimate discovery whenever a target happens to
        // also belong to the starting set (e.g. one Furniture item's parent
        // is itself categorized as Furniture). The starting objects are
        // instead excluded from the output afterward, by identity.
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> starting_ids;
        for (const auto &item : items)
        {
            std::string id = item.value("uniqueId", "");
            if (!id.empty())
                starting_ids.insert(id);
        }

        json result = json::array();
        json frontier = items;

        for (int64_t depth = 1; depth <= effective_max_depth; ++depth)
        {
            json edges = json::array();
            json next_frontier = json::array();

            for (const auto &item : frontier)
            {
                std::string from_id = item.value("uniqueId", "");

                if (reverse)
                {
                    json matches = traverse_relationship_reverse(ctx, item, field);

                    for (const auto &match : matches)
                    {
                        std::string to_id = match.value("uniqueId", "");
                        edges.push_back({{"from", from_id}, {"to", to_id}});

                        if (!to_id.empty() && visited.insert(to_id).second)
                            next_frontier.push_back(match);
                    }
                }
                else
                {
                    const json *related = traverse_relationship(ctx, item, field);
                    if (!related)
                        continue;

                    std::string to_id = related->value("uniqueId", "");
                    edges.push_back({{"from", from_id}, {"to", to_id}});

                    if (!to_id.empty() && visited.insert(to_id).second)
                        next_frontier.push_back(*related);
                }
            }

            if (next_frontier.empty())
                break;

            paths_out.push_back({{"relationship", field},
                                  {"direction", reverse ? "reverse" : "forward"},
                                  {"depth", depth},
                                  {"edges", edges}});

            for (const auto &obj : next_frontier)
                result.push_back(obj);

            frontier = next_frontier;
        }

        // A cycle can lead back to one of the original starting objects;
        // exclude those from the output so results are strictly "newly
        // reached objects," never an echo of what the walk started with.
        json filtered_result = json::array();
        for (const auto &obj : result)
        {
            std::string id = obj.value("uniqueId", "");
            if (starting_ids.find(id) == starting_ids.end())
                filtered_result.push_back(obj);
        }

        return filtered_result;
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
            if (ast.contains("traverse_tree")) fwd["traverse_tree"] = ast["traverse_tree"];
            if (ast.contains("traverse_recursive")) fwd["traverse_recursive"] = ast["traverse_recursive"];
            if (ast.contains("include_paths")) fwd["include_paths"] = ast["include_paths"];
            if (ast.contains("filters_after_traverse")) fwd["filters_after_traverse"] = ast["filters_after_traverse"];
            if (ast.contains("select")) fwd["select"] = ast["select"];
            if (ast.contains("aggregate")) fwd["aggregate"] = ast["aggregate"];
            if (ast.contains("constraints")) fwd["constraints"] = ast["constraints"];
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

        if (op == "set_query")
        {
            json fwd = {
                {"action", "set_query"},
                {"model", model},
                {"expression", ast.value("expression", json::object())}};

            if (ast.contains("include_paths")) fwd["include_paths"] = ast["include_paths"];
            if (ast.contains("select")) fwd["select"] = ast["select"];
            if (ast.contains("aggregate")) fwd["aggregate"] = ast["aggregate"];
            if (ast.contains("constraints")) fwd["constraints"] = ast["constraints"];
            if (ast.contains("distinct")) fwd["distinct"] = ast["distinct"];
            if (ast.contains("group_by")) fwd["group_by"] = ast["group_by"];
            if (ast.contains("sort")) fwd["sort"] = ast["sort"];
            if (ast.contains("limit")) fwd["limit"] = ast["limit"];

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
    using FallbackStep = const json *(*)(const SemanticContext &ctx, const json &item);

    // instance -> type: follows the instance's typeId to its type record
    // (the "types" bucket in semantic.json), reusing the existing generic
    // whole-tree uniqueId lookup already used by the traversal-chain feature.
    static const json *fallback_via_type(const SemanticContext &ctx, const json &item)
    {
        // Same lookup as traverse_relationship's "typeId" case; this is the
        // one sanctioned use of that classification link (see
        // classification_link_fields()) -- parameter inheritance, not graph
        // traversal.
        return traverse_relationship(ctx, item, "typeId");
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
    static const json *resolve_with_fallback(const SemanticContext &ctx, const json &item, const std::string &field_or_path, FieldResolver base)
    {
        const json *direct = base(item, field_or_path);
        if (direct)
            return direct;

        for (auto step : parameter_fallback_chain())
        {
            const json *related = step(ctx, item);
            if (!related)
                continue;

            const json *found = base(*related, field_or_path);
            if (found)
                return found;
        }

        return nullptr;
    }

    static const json *resolve_field_with_fallback(const SemanticContext &ctx, const json &item, const std::string &field)
    {
        return resolve_with_fallback(ctx, item, field, resolve_field);
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
    static bool eval_filter_clause(const SemanticContext &ctx, const json &item, const json &clause)
    {
        if (!clause.is_object())
            return false;

        std::string field = clause.value("field", "");
        std::string op = clause.value("op", "eq");

        if (field.empty())
            return false;

        const json *actual = resolve_field_with_fallback(ctx, item, field);

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
    static bool eval_filters(const SemanticContext &ctx, const json &item, const json &filters, const std::string &match)
    {
        if (!filters.is_array() || filters.empty())
            return true;

        bool require_all = (match != "any");

        for (const auto &clause : filters)
        {
            bool clause_result = eval_filter_clause(ctx, item, clause);

            if (require_all && !clause_result)
                return false;

            if (!require_all && clause_result)
                return true;
        }

        return require_all;
    }

    // Applies a filter stage (the same eval_filters/eval_filter_clause engine
    // above) to an arbitrary result set rather than the whole tree. Step 3
    // (relationship-aware filtering) calls this once, right after
    // traversal, so a filter can be re-evaluated on the objects traversal
    // just produced; a future "filter -> traverse -> filter -> traverse ->
    // filter" pipeline would just call this (and apply_traversal_chain)
    // repeatedly in sequence -- neither needs to change for that.
    static json apply_filters_stage(const SemanticContext &ctx, const json &items, const json &filters, const std::string &match)
    {
        json out = json::array();

        for (const auto &item : items)
            if (eval_filters(ctx, item, filters, match))
                out.push_back(item);

        return out;
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
    static const json *resolve_path_with_fallback(const SemanticContext &ctx, const json &item, const std::string &path)
    {
        return resolve_with_fallback(ctx, item, path, resolve_path);
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
    static json project_fields(const SemanticContext &ctx, const json &item, const std::vector<std::string> &fields)
    {
        json out = json::object();

        for (const auto &f : fields)
        {
            const json *v = resolve_path_with_fallback(ctx, item, f);
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
    static void sort_results(const SemanticContext &ctx, json &arr, const std::string &field, bool descending)
    {
        if (!arr.is_array() || field.empty())
            return;

        std::vector<json> items(arr.begin(), arr.end());

        std::stable_sort(items.begin(), items.end(), [&](const json &lhs, const json &rhs)
                          {
            const json *lv = resolve_path_with_fallback(ctx, lhs, field);
            const json *rv = resolve_path_with_fallback(ctx, rhs, field);

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
    static json group_by_results(const SemanticContext &ctx, const json &items, const std::string &field)
    {
        std::vector<std::string> order;
        std::unordered_map<std::string, int> counts;
        std::unordered_map<std::string, json> values;

        for (const auto &item : items)
        {
            const json *v = resolve_path_with_fallback(ctx, item, field);
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
    static json distinct_results(const SemanticContext &ctx, const json &items, const std::vector<std::string> &fields)
    {
        std::unordered_map<std::string, bool> seen;
        json out = json::array();

        for (const auto &item : items)
        {
            std::string key;
            for (const auto &f : fields)
            {
                const json *v = resolve_path_with_fallback(ctx, item, f);
                key += json_to_compare_string(v ? *v : json(nullptr));
                key += '\x1f';
            }

            if (seen.find(key) != seen.end())
                continue;

            seen[key] = true;
            out.push_back(project_fields(ctx, item, fields));
        }

        return out;
    }

    // select: projects every item down to just `fields`, one output entry
    // per matched item (not deduplicated).
    static json select_results(const SemanticContext &ctx, const json &items, const std::vector<std::string> &fields)
    {
        json out = json::array();

        for (const auto &item : items)
            out.push_back(project_fields(ctx, item, fields));

        return out;
    }

    // ------------------------------------------------------------------
    // Step 6: graph aggregation.
    //
    // Aggregation runs after filtering/traversal (whatever "results"
    // currently holds, regardless of how it got there) and never modifies
    // eval_filters/eval_filter_clause or the traversal engine -- it only
    // consumes traverse_relationship*/apply_traversal_chain to resolve a
    // graph-based group_by, exactly the same functions "traverse" already
    // uses.
    // ------------------------------------------------------------------

    static const std::unordered_set<std::string> &valid_aggregate_functions()
    {
        static const std::unordered_set<std::string> functions = {"count", "sum", "min", "max", "avg"};
        return functions;
    }

    // One aggregation metric: which function to compute, over which field
    // (unused for count), with an explicit or default output key ("count",
    // or "<function>_<field>" when not aliased via "as").
    struct AggregateMetric
    {
        std::string function;
        std::string field;
        std::string alias;
    };

    static std::vector<AggregateMetric> parse_aggregate_metrics(const json &v)
    {
        std::vector<AggregateMetric> metrics;

        if (!v.is_array())
            return metrics;

        for (const auto &m : v)
        {
            if (!m.is_object())
                continue;

            AggregateMetric metric;
            metric.function = m.value("function", "");
            metric.field = m.value("field", "");
            metric.alias = m.value("as", metric.function == "count" ? "count" : (metric.function + "_" + metric.field));

            metrics.push_back(metric);
        }

        return metrics;
    }

    // Resolves the graph-based group identity/identities for one item: runs
    // the given relationship chain through apply_traversal_chain exactly as
    // "traverse" does (starting from a single-item set), then returns a
    // lightweight identifier -- uniqueId, category, and intId when the node
    // has one -- per node reached, never the full object. A chain step that
    // fans out (a reverse hop reaching several nodes) makes this item
    // contribute to every reached node's group, the same fan-out reverse
    // traversal already has elsewhere.
    static json graph_group_identities(const SemanticContext &ctx, const json &item, const std::vector<TraversalStep> &chain)
    {
        json single = json::array();
        single.push_back(item);

        json reached = apply_traversal_chain(ctx, single, chain);

        json identities = json::array();

        for (const auto &node : reached)
        {
            json identity = {{"uniqueId", node.value("uniqueId", "")},
                              {"category", node.value("category", json(nullptr))}};

            if (node.contains("intId"))
                identity["intId"] = node["intId"];

            identities.push_back(identity);
        }

        return identities;
    }

    // Running per-group state for one or more metrics.
    struct AggregateAccumulator
    {
        json group_value;
        int64_t count = 0;
        std::vector<double> sums;
        std::vector<int64_t> numeric_counts;
        std::vector<bool> has_min_max;
        std::vector<double> mins;
        std::vector<double> maxs;
    };

    // Computes count/sum/min/max/avg over `items`, optionally grouped.
    // group_by is plain-attribute (resolved via resolve_path_with_fallback,
    // same resolver select/distinct/sort already use) when `group_is_graph`
    // is false, or graph-based (resolved via graph_group_identities, i.e.
    // real traversal) when true. Metrics always resolve against the item's
    // own attributes via resolve_path_with_fallback and json_value_to_number
    // -- the same field resolution and numeric coercion used everywhere
    // else in this engine, not reimplemented here.
    static json aggregate_results(const SemanticContext &ctx, const json &items, bool has_group, bool group_is_graph,
                                   const std::string &group_field, const std::vector<TraversalStep> &group_chain,
                                   const std::vector<AggregateMetric> &metrics)
    {
        std::vector<std::string> order;
        std::unordered_map<std::string, AggregateAccumulator> groups;

        auto ensure_group = [&](const std::string &key, const json &group_value) -> AggregateAccumulator &
        {
            auto it = groups.find(key);
            if (it != groups.end())
                return it->second;

            order.push_back(key);
            AggregateAccumulator &acc = groups[key];
            acc.group_value = group_value;
            acc.sums.assign(metrics.size(), 0.0);
            acc.numeric_counts.assign(metrics.size(), 0);
            acc.has_min_max.assign(metrics.size(), false);
            acc.mins.assign(metrics.size(), 0.0);
            acc.maxs.assign(metrics.size(), 0.0);
            return acc;
        };

        auto accumulate = [&](AggregateAccumulator &acc, const json &item)
        {
            acc.count++;

            for (size_t i = 0; i < metrics.size(); ++i)
            {
                const auto &m = metrics[i];
                if (m.function == "count")
                    continue;

                const json *v = resolve_path_with_fallback(ctx, item, m.field);
                double num;
                if (!v || !json_value_to_number(*v, num))
                    continue;

                acc.sums[i] += num;
                acc.numeric_counts[i] += 1;

                if (!acc.has_min_max[i])
                {
                    acc.mins[i] = num;
                    acc.maxs[i] = num;
                    acc.has_min_max[i] = true;
                }
                else
                {
                    acc.mins[i] = std::min(acc.mins[i], num);
                    acc.maxs[i] = std::max(acc.maxs[i], num);
                }
            }
        };

        for (const auto &item : items)
        {
            if (!has_group)
            {
                accumulate(ensure_group("", json(nullptr)), item);
                continue;
            }

            if (group_is_graph)
            {
                json identities = graph_group_identities(ctx, item, group_chain);

                for (const auto &identity : identities)
                {
                    std::string key = identity.value("uniqueId", "");
                    accumulate(ensure_group(key, identity), item);
                }

                continue;
            }

            const json *v = resolve_path_with_fallback(ctx, item, group_field);
            json group_value = v ? *v : json(nullptr);
            std::string key = json_to_compare_string(group_value);
            accumulate(ensure_group(key, group_value), item);
        }

        json out = json::array();

        for (const auto &key : order)
        {
            const AggregateAccumulator &acc = groups[key];
            json row = json::object();

            if (has_group)
                row["group"] = acc.group_value;

            for (size_t i = 0; i < metrics.size(); ++i)
            {
                const auto &m = metrics[i];

                if (m.function == "count")
                    row[m.alias] = acc.count;
                else if (m.function == "sum")
                    row[m.alias] = acc.sums[i];
                else if (m.function == "avg")
                    row[m.alias] = acc.numeric_counts[i] > 0 ? json(acc.sums[i] / acc.numeric_counts[i]) : json(nullptr);
                else if (m.function == "min")
                    row[m.alias] = acc.has_min_max[i] ? json(acc.mins[i]) : json(nullptr);
                else if (m.function == "max")
                    row[m.alias] = acc.has_min_max[i] ? json(acc.maxs[i]) : json(nullptr);
            }

            out.push_back(row);
        }

        return out;
    }

    // ------------------------------------------------------------------
    // Step 9: constraint evaluation.
    //
    // Unlike filtering, this never removes objects -- every evaluated
    // object is returned, annotated with a pass/fail verdict per
    // constraint. Two kinds of constraint:
    //   - field constraint: {"field","op","value"} -- handed straight to
    //     eval_filter_clause, unmodified, the exact same function filters
    //     already use.
    //   - graph constraint: {"traverse_tree": ..., "where": [...], "op",
    //     "value"} -- the SAME traverse_tree grammar and parser/validator/
    //     executor ("parse_traversal_forest"/"validate_traversal_forest"/
    //     "apply_traversal_forest") the top-level traverse_tree request
    //     field already uses, run on a single-item set (same trick Step 6's
    //     graph_group_identities uses), then the reached set's size is
    //     compared against `value`. "where" (optional) narrows the reached
    //     set first via apply_filters_stage, unmodified.
    // Does not modify eval_filters/eval_filter_clause, the traversal
    // engine, aggregate_results, or the set-operation functions -- only
    // consumes them.
    // ------------------------------------------------------------------

    static const std::unordered_set<std::string> &valid_constraint_count_ops()
    {
        static const std::unordered_set<std::string> ops = {"eq", "neq", "gt", "gte", "lt", "lte", "exists", "not_exists"};
        return ops;
    }

    struct Constraint
    {
        std::string name;
        bool is_graph;

        json field_clause; // field constraint: the raw {field,op,value} object

        std::vector<TraversalNode> tree; // graph constraint: parsed traverse_tree
        json where_filters;
        std::string count_op;
        double count_value;
    };

    static std::vector<Constraint> parse_constraints(const json &v)
    {
        std::vector<Constraint> out;

        if (!v.is_array())
            return out;

        int auto_index = 0;
        for (const auto &c : v)
        {
            if (!c.is_object())
                continue;

            Constraint constraint;
            constraint.name = c.value("name", "constraint_" + std::to_string(auto_index));
            auto_index++;

            if (c.contains("traverse_tree"))
            {
                constraint.is_graph = true;
                constraint.tree = parse_traversal_forest(c["traverse_tree"]);
                constraint.where_filters = (c.contains("where") && c["where"].is_array()) ? c["where"] : json::array();
                constraint.count_op = c.value("op", "gte");
                constraint.count_value = c.value("value", 1.0);
            }
            else
            {
                constraint.is_graph = false;
                constraint.field_clause = c;
            }

            out.push_back(constraint);
        }

        return out;
    }

    // Validates every constraint: graph constraints' traverse_tree is
    // validated with the exact same validate_traversal_forest used for the
    // top-level traverse_tree field, and the count-comparison operator is
    // checked against valid_constraint_count_ops. Field constraints aren't
    // separately validated here -- eval_filter_clause already treats an
    // unrecognized op as "never matches" rather than erroring, and that
    // established behavior isn't something constraint evaluation should
    // second-guess.
    static std::string validate_constraints(const json &sem, const std::vector<Constraint> &constraints)
    {
        for (const auto &c : constraints)
        {
            if (!c.is_graph)
                continue;

            std::string tree_error = validate_traversal_forest(sem, c.tree);
            if (!tree_error.empty())
                return tree_error;

            if (!valid_constraint_count_ops().count(c.count_op))
                return "Unknown constraint operator: \"" + c.count_op + "\"";
        }

        return "";
    }

    static bool evaluate_graph_constraint(const SemanticContext &ctx, const json &item, const Constraint &c)
    {
        json single = json::array();
        single.push_back(item);

        json unused_paths = json::array();
        json reached = apply_traversal_forest(ctx, single, c.tree, unused_paths);

        if (!c.where_filters.empty())
            reached = apply_filters_stage(ctx, reached, c.where_filters, "all");

        double count = static_cast<double>(reached.size());

        if (c.count_op == "exists")
            return count > 0;
        if (c.count_op == "not_exists")
            return count == 0;
        if (c.count_op == "eq")
            return count == c.count_value;
        if (c.count_op == "neq")
            return count != c.count_value;
        if (c.count_op == "gt")
            return count > c.count_value;
        if (c.count_op == "gte")
            return count >= c.count_value;
        if (c.count_op == "lt")
            return count < c.count_value;
        if (c.count_op == "lte")
            return count <= c.count_value;

        return false;
    }

    // Evaluates every constraint against every item, returning a full copy
    // of each item (nothing removed, nothing reshaped) with an added
    // "constraints" object mapping each constraint's name to its pass/fail
    // verdict for that item.
    static json evaluate_constraints(const SemanticContext &ctx, const json &items, const std::vector<Constraint> &constraints)
    {
        json out = json::array();

        for (const auto &item : items)
        {
            json annotated = item;
            json verdicts = json::object();

            for (const auto &c : constraints)
            {
                bool pass = c.is_graph
                                ? evaluate_graph_constraint(ctx, item, c)
                                : eval_filter_clause(ctx, item, c.field_clause);
                verdicts[c.name] = pass;
            }

            annotated["constraints"] = verdicts;
            out.push_back(annotated);
        }

        return out;
    }

    // Applies the shape/aggregate -> sort -> limit stages to an
    // already-computed results array and builds the final response object
    // on top of `base_response` (which already carries whatever envelope
    // fields the caller owns, e.g. {"action","match","filters"} for a
    // normal query or {"action":"set_query"} for a combined set-operation
    // result). Used both by execute_semantic_filter_query (after its own
    // filter+traverse+filters_after_traverse stage) and directly by
    // handle_action_set_query (Step 8) on a combined set-operation result --
    // neither has anything to do with HOW `results` was produced, so this
    // function doesn't need to know either. This is the exact tail that
    // used to live inline in handle_action_semantic_filter; behavior is
    // unchanged for that caller, just relocated so a second caller can
    // reuse it without duplicating the shape/sort/limit dispatch.
    static json apply_post_processing_and_respond(const SemanticContext &ctx, const json &results, const json &req, json base_response)
    {
        bool has_group_by = req.contains("group_by") && req["group_by"].is_string() && !req["group_by"].get<std::string>().empty();
        bool has_distinct = req.contains("distinct") && !to_field_list(req["distinct"]).empty();
        bool has_select = req.contains("select") && !to_field_list(req["select"]).empty();
        bool has_aggregate = req.contains("aggregate") && req["aggregate"].is_object();
        bool has_constraints = req.contains("constraints") && req["constraints"].is_array() && !req["constraints"].empty();

        int shape_count = (has_group_by ? 1 : 0) + (has_distinct ? 1 : 0) + (has_select ? 1 : 0) + (has_aggregate ? 1 : 0) + (has_constraints ? 1 : 0);
        if (shape_count > 1)
            return json({{"error", "group_by, distinct, select, aggregate, and constraints are mutually exclusive"}});

        json shaped = results;

        if (has_group_by)
            shaped = group_by_results(ctx, results, req["group_by"].get<std::string>());
        else if (has_distinct)
            shaped = distinct_results(ctx, results, to_field_list(req["distinct"]));
        else if (has_select)
            shaped = select_results(ctx, results, to_field_list(req["select"]));
        else if (has_aggregate)
        {
            const json &agg = req["aggregate"];

            bool agg_has_group = agg.contains("group_by") && !agg["group_by"].is_null();
            bool agg_group_is_graph = agg_has_group && agg["group_by"].is_object() && agg["group_by"].contains("relationship");
            std::string agg_group_field;
            std::vector<TraversalStep> agg_group_chain;

            if (agg_has_group)
            {
                if (agg_group_is_graph)
                {
                    agg_group_chain = to_traversal_steps(agg["group_by"]["relationship"]);

                    std::string chain_error = validate_traversal_steps(ctx.tree, agg_group_chain);
                    if (!chain_error.empty())
                        return json({{"error", chain_error}});
                }
                else if (agg["group_by"].is_string())
                {
                    agg_group_field = agg["group_by"].get<std::string>();
                }
                else
                {
                    return json({{"error", "aggregate.group_by must be a field name string or {\"relationship\": ...}"}});
                }
            }

            std::vector<AggregateMetric> metrics = parse_aggregate_metrics(agg.value("metrics", json::array()));

            for (const auto &m : metrics)
            {
                if (!valid_aggregate_functions().count(m.function))
                    return json({{"error", "Unknown aggregate function: \"" + m.function + "\""}});
            }

            shaped = aggregate_results(ctx, results, agg_has_group, agg_group_is_graph, agg_group_field, agg_group_chain, metrics);
        }
        else if (has_constraints)
        {
            std::vector<Constraint> constraints = parse_constraints(req["constraints"]);

            std::string constraint_error = validate_constraints(ctx.tree, constraints);
            if (!constraint_error.empty())
                return json({{"error", constraint_error}});

            shaped = evaluate_constraints(ctx, results, constraints);
        }

        bool has_sort = req.contains("sort") && req["sort"].is_object();
        if (has_sort)
        {
            std::string sort_field = req["sort"].value("field", "");
            std::string sort_order = req["sort"].value("order", "asc");
            sort_results(ctx, shaped, sort_field, sort_order == "desc");
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

        base_response["count"] = shaped_count;
        base_response["results"] = shaped;

        if (has_group_by)
            base_response["group_by"] = req["group_by"];
        if (has_distinct)
            base_response["distinct"] = req["distinct"];
        if (has_select)
            base_response["select"] = req["select"];
        if (has_aggregate)
            base_response["aggregate"] = req["aggregate"];
        if (has_constraints)
            base_response["constraints"] = req["constraints"];
        if (has_sort)
            base_response["sort"] = req["sort"];
        if (has_limit)
            base_response["limit"] = req["limit"];

        return base_response;
    }

    // The full semantic_filter pipeline -- filter walk, traverse/traverse_tree,
    // filters_after_traverse, then apply_post_processing_and_respond -- given
    // an ALREADY-LOADED semantic tree, returning the response as a json
    // object directly (no serialization). This is the one place the pipeline
    // actually runs; handle_action_semantic_filter (below) and Step 8's set-
    // query leaves both call it directly, in-process -- leaves never
    // serialize to a string or re-enter the action dispatcher.
    static json execute_semantic_filter_query(const SemanticContext &ctx, const json &req)
    {
        json filters = (req.contains("filters") && req["filters"].is_array())
                           ? req["filters"]
                           : json::array();

        std::string match = req.value("match", "all");

        json results = json::array();

        // Generic walk: every top-level bucket, every item inside its
        // "items" array, regardless of bucket name or item category.
        if (ctx.tree.is_object())
        {
            for (const auto &bucket_entry : ctx.tree.items())
            {
                const json &bucket = bucket_entry.value();

                if (!bucket.is_object() || !bucket.contains("items") || !bucket["items"].is_array())
                    continue;

                for (const auto &item : bucket["items"])
                {
                    if (!item.is_object())
                        continue;

                    if (eval_filters(ctx, item, filters, match))
                        results.push_back(item);
                }
            }
        }

        std::vector<TraversalStep> traverse_chain = to_traversal_steps(req.value("traverse", json::array()));
        bool has_traverse = !traverse_chain.empty();
        bool has_traverse_tree = req.contains("traverse_tree") && !req["traverse_tree"].is_null();
        bool has_traverse_recursive = req.contains("traverse_recursive") && req["traverse_recursive"].is_object();
        bool include_paths = req.value("include_paths", false);
        json paths = json::array();

        int traversal_mode_count = (has_traverse ? 1 : 0) + (has_traverse_tree ? 1 : 0) + (has_traverse_recursive ? 1 : 0);
        if (traversal_mode_count > 1)
            return json({{"error", "traverse, traverse_tree, and traverse_recursive are mutually exclusive"}});

        if (has_traverse)
        {
            std::string chain_error = validate_traversal_steps(ctx.tree, traverse_chain);
            if (!chain_error.empty())
                return json({{"error", chain_error}});

            results = include_paths
                          ? apply_traversal_chain_with_paths(ctx, results, traverse_chain, paths)
                          : apply_traversal_chain(ctx, results, traverse_chain);
        }
        else if (has_traverse_tree)
        {
            std::vector<TraversalNode> forest = parse_traversal_forest(req["traverse_tree"]);

            std::string tree_error = validate_traversal_forest(ctx.tree, forest);
            if (!tree_error.empty())
                return json({{"error", tree_error}});

            results = apply_traversal_forest(ctx, results, forest, paths);
        }
        else if (has_traverse_recursive)
        {
            const json &rec = req["traverse_recursive"];
            std::string field = rec.value("field", "");

            if (field.empty())
                return json({{"error", "traverse_recursive requires a \"field\""}});

            bool reverse = rec.value("direction", "forward") == "reverse";

            // -1 is the "omitted" sentinel, distinct from an explicit 0 (zero
            // hops) -- .value()'s single-default form can't tell those apart.
            int64_t max_depth = -1;
            if (rec.contains("max_depth") && !rec["max_depth"].is_null())
            {
                if (!rec["max_depth"].is_number_integer())
                    return json({{"error", "traverse_recursive \"max_depth\" must be an integer"}});

                max_depth = rec["max_depth"].get<int64_t>();
                if (max_depth < 0)
                    return json({{"error", "traverse_recursive \"max_depth\" must be >= 0"}});
            }

            std::vector<TraversalStep> single_step = {{field, reverse}};
            std::string rel_error = validate_traversal_steps(ctx.tree, single_step);
            if (!rel_error.empty())
                return json({{"error", rel_error}});

            results = apply_recursive_traversal(ctx, results, field, reverse, max_depth, paths);
        }

        json filters_after_traverse = (req.contains("filters_after_traverse") && req["filters_after_traverse"].is_array())
                                           ? req["filters_after_traverse"]
                                           : json::array();
        bool has_filters_after_traverse = !filters_after_traverse.empty();

        if (has_filters_after_traverse)
            results = apply_filters_stage(ctx, results, filters_after_traverse, "all");

        json base = {{"plugin", "AIQuery"},
                     {"status", "ok"},
                     {"action", "semantic_filter"},
                     {"match", match},
                     {"filters", filters}};

        if (has_traverse)
            base["traverse"] = req["traverse"];
        if (has_traverse_tree)
            base["traverse_tree"] = req["traverse_tree"];
        if (has_traverse_recursive)
            base["traverse_recursive"] = req["traverse_recursive"];
        if ((has_traverse || has_traverse_tree || has_traverse_recursive) && include_paths)
            base["paths"] = paths;
        if (has_filters_after_traverse)
            base["filters_after_traverse"] = filters_after_traverse;

        return apply_post_processing_and_respond(ctx, results, req, base);
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

        std::unordered_map<std::string, const json *> id_index = build_unique_id_index(sem);
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const json *>>> reverse_index_cache;
        SemanticContext ctx{sem, id_index, reverse_index_cache};

        response = execute_semantic_filter_query(ctx, req).dump();
        return true;
    }

    // ------------------------------------------------------------------
    // Step 8: set operations.
    //
    // Combines the object sets produced by independent subqueries (union/
    // intersect/difference), each subquery executed via
    // execute_semantic_filter_query directly -- no re-entry into the
    // public action dispatcher, no string serialization between leaves.
    // Does not modify eval_filters/eval_filter_clause, the traversal engine,
    // or aggregate_results -- it only ever consumes their output through
    // execute_semantic_filter_query.
    // ------------------------------------------------------------------

    static json set_union(const std::vector<json> &operand_sets)
    {
        std::unordered_set<std::string> seen;
        json out = json::array();

        for (const auto &set : operand_sets)
            for (const auto &obj : set)
            {
                std::string id = obj.value("uniqueId", "");
                if (id.empty() || seen.insert(id).second)
                    out.push_back(obj);
            }

        return out;
    }

    static json set_intersect(const std::vector<json> &operand_sets)
    {
        json out = json::array();
        if (operand_sets.empty())
            return out;

        std::vector<std::unordered_set<std::string>> id_sets;
        for (const auto &set : operand_sets)
        {
            std::unordered_set<std::string> ids;
            for (const auto &obj : set)
                ids.insert(obj.value("uniqueId", ""));
            id_sets.push_back(ids);
        }

        std::unordered_set<std::string> seen;
        for (const auto &obj : operand_sets[0])
        {
            std::string id = obj.value("uniqueId", "");
            if (id.empty() || seen.count(id))
                continue;

            bool in_all = true;
            for (size_t i = 1; i < id_sets.size(); ++i)
            {
                if (!id_sets[i].count(id))
                {
                    in_all = false;
                    break;
                }
            }

            if (in_all)
            {
                seen.insert(id);
                out.push_back(obj);
            }
        }

        return out;
    }

    // operands[0] minus the union of every other operand.
    static json set_difference(const std::vector<json> &operand_sets)
    {
        json out = json::array();
        if (operand_sets.empty())
            return out;

        std::unordered_set<std::string> excluded;
        for (size_t i = 1; i < operand_sets.size(); ++i)
            for (const auto &obj : operand_sets[i])
                excluded.insert(obj.value("uniqueId", ""));

        std::unordered_set<std::string> seen;
        for (const auto &obj : operand_sets[0])
        {
            std::string id = obj.value("uniqueId", "");
            if (id.empty() || seen.count(id) || excluded.count(id))
                continue;

            seen.insert(id);
            out.push_back(obj);
        }

        return out;
    }

    // Recursively prunes a Step-5 (traverse_tree) shaped path node's edges,
    // and its "then" children if any, so only edges on a chain to a
    // surviving final object remain. Returns the set of "from" ids that
    // survived at this node -- which of its own source objects still lead
    // somewhere that matters -- for the caller (parent node) to decide
    // which of ITS OWN edges to keep. A node with no "then" is a leaf hop
    // and prunes directly against the final survivor set.
    static std::unordered_set<std::string> prune_tree_path_node(json &node, const std::unordered_set<std::string> &final_survivors)
    {
        std::unordered_set<std::string> required_from;

        if (node.contains("then") && node["then"].is_array() && !node["then"].empty())
        {
            std::unordered_set<std::string> needed_targets;

            for (auto &child : node["then"])
            {
                std::unordered_set<std::string> child_from = prune_tree_path_node(child, final_survivors);
                needed_targets.insert(child_from.begin(), child_from.end());
            }

            json kept_edges = json::array();
            for (const auto &edge : node["edges"])
            {
                std::string to = edge.value("to", "");
                if (needed_targets.count(to))
                {
                    kept_edges.push_back(edge);
                    required_from.insert(edge.value("from", ""));
                }
            }
            node["edges"] = kept_edges;
        }
        else
        {
            json kept_edges = json::array();
            for (const auto &edge : node["edges"])
            {
                std::string to = edge.value("to", "");
                if (final_survivors.count(to))
                {
                    kept_edges.push_back(edge);
                    required_from.insert(edge.value("from", ""));
                }
            }
            node["edges"] = kept_edges;
        }

        return required_from;
    }

    static void prune_tree_paths(json &paths, const std::unordered_set<std::string> &final_survivors)
    {
        for (auto &root : paths)
            prune_tree_path_node(root, final_survivors);
    }

    // Prunes a Step-4 (flat chain) shaped paths array backward: the last hop
    // is pruned against the final survivor set, then each earlier hop is
    // pruned against the set of "from" values the NEXT hop actually kept --
    // "which hop feeds which" is implicit in array order for this shape,
    // unlike the tree shape's explicit "then" nesting.
    static void prune_flat_paths(json &paths, const std::unordered_set<std::string> &final_survivors)
    {
        std::unordered_set<std::string> required_to = final_survivors;

        for (int i = static_cast<int>(paths.size()) - 1; i >= 0; --i)
        {
            json &hop = paths[static_cast<size_t>(i)];
            json kept_edges = json::array();
            std::unordered_set<std::string> required_from;

            for (const auto &edge : hop["edges"])
            {
                std::string to = edge.value("to", "");
                if (required_to.count(to))
                {
                    kept_edges.push_back(edge);
                    required_from.insert(edge.value("from", ""));
                }
            }

            hop["edges"] = kept_edges;
            required_to = required_from;
        }
    }

    // Executes one node of a set expression (a leaf query or a nested set
    // operation), returning either {"error": "..."} or a plain array of
    // semantic objects. Leaves execute via execute_semantic_filter_query
    // directly, in-process. Leaf path output (if requested) is collected
    // into leaf_paths_out, tagged by an incrementing leaf index, for
    // pruning against the FINAL survivor set once the whole tree resolves
    // (handle_action_set_query does the pruning, not this function, since
    // "final" only means anything once every operand has run).
    static json execute_set_operand(const SemanticContext &ctx, const json &node, bool force_include_paths, json &leaf_paths_out, int &next_leaf_index)
    {
        bool is_set_op = node.is_object() && node.contains("operation") &&
                         (node["operation"] == "union" || node["operation"] == "intersect" || node["operation"] == "difference");

        if (is_set_op)
        {
            std::string op = node.value("operation", "");

            if (!node.contains("operands") || !node["operands"].is_array() || node["operands"].empty())
                return json({{"error", "set operation \"" + op + "\" requires a non-empty operands array"}});

            std::vector<json> operand_sets;
            for (const auto &child : node["operands"])
            {
                json result = execute_set_operand(ctx, child, force_include_paths, leaf_paths_out, next_leaf_index);
                if (result.is_object() && result.contains("error"))
                    return result;
                operand_sets.push_back(result);
            }

            if (op == "union")
                return set_union(operand_sets);
            if (op == "intersect")
                return set_intersect(operand_sets);
            return set_difference(operand_sets);
        }

        // Leaf: an ordinary query object, executed via the exact same
        // pipeline any other query uses.
        json leaf_req = node;
        if (force_include_paths)
            leaf_req["include_paths"] = true;

        json leaf_response = execute_semantic_filter_query(ctx, leaf_req);

        if (leaf_response.contains("error"))
            return leaf_response;

        if (!leaf_response.contains("results") || !leaf_response["results"].is_array())
            return json({{"error", "set operation operand did not produce a result array"}});

        for (const auto &obj : leaf_response["results"])
        {
            if (!obj.is_object() || !obj.contains("uniqueId"))
                return json({{"error", "set operation operands must produce semantic objects (with uniqueId), not select/group_by/distinct/aggregate rows"}});
        }

        if (leaf_response.contains("paths"))
        {
            json entry = {{"leaf", next_leaf_index}, {"paths", leaf_response["paths"]}};
            entry["shape"] = leaf_response.contains("traverse_tree") ? "tree" : "flat";
            leaf_paths_out.push_back(entry);
        }

        next_leaf_index++;

        return leaf_response["results"];
    }

    static bool handle_action_set_query(const json &req, const std::string &model, std::string &response)
    {
        std::string action = req.value("action", "");
        if (action != "set_query")
            return false;

        if (!req.contains("expression") || !req["expression"].is_object())
        {
            response = json({{"error", "missing expression"}}).dump();
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

        std::unordered_map<std::string, const json *> id_index = build_unique_id_index(sem);
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const json *>>> reverse_index_cache;
        SemanticContext ctx{sem, id_index, reverse_index_cache};

        bool include_paths = req.value("include_paths", false);
        json leaf_paths = json::array();
        int next_leaf_index = 0;

        json combined = execute_set_operand(ctx, req["expression"], include_paths, leaf_paths, next_leaf_index);

        if (combined.is_object() && combined.contains("error"))
        {
            response = combined.dump();
            return true;
        }

        json base = {{"plugin", "AIQuery"}, {"status", "ok"}, {"action", "set_query"}};

        if (include_paths && !leaf_paths.empty())
        {
            std::unordered_set<std::string> final_survivors;
            for (const auto &obj : combined)
                final_survivors.insert(obj.value("uniqueId", ""));

            for (auto &entry : leaf_paths)
            {
                if (entry.value("shape", "") == "tree")
                    prune_tree_paths(entry["paths"], final_survivors);
                else
                    prune_flat_paths(entry["paths"], final_survivors);
            }

            base["paths"] = leaf_paths;
        }

        response = apply_post_processing_and_respond(ctx, combined, req, base).dump();
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

        std::unordered_map<std::string, const json *> id_index = build_unique_id_index(sem);
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const json *>>> reverse_index_cache;
        SemanticContext ctx{sem, id_index, reverse_index_cache};

        if (sem_loaded && !semantic_unique_id.empty())
        {
            const json *found_elem = find_semantic_by_unique_id(ctx, semantic_unique_id);

            if (found_elem)
            {
                semantic_ok = true;
                selected_element = *found_elem;
                resolved_element = *found_elem;

                traversal_path.push_back(resolved_element.value("uniqueId", ""));

                for (int depth = 0; depth < 32; ++depth)
                {
                    const json *next = find_next_semantic_context(ctx, resolved_element);
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
        bool sem_loaded = load_json_mmap(sem_path, sem);
        std::unordered_map<std::string, const json *> id_index = build_unique_id_index(sem);
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const json *>>> reverse_index_cache;
        SemanticContext ctx{sem, id_index, reverse_index_cache};

        if (sem_loaded && !semantic_unique_id.empty())
        {
            const json *found_elem = find_semantic_by_unique_id(ctx, semantic_unique_id);

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

            if (handle_action_set_query(req, model, response))
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