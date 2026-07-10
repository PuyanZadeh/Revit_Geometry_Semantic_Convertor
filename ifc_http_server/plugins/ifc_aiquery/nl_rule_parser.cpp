#include "nl_rule_parser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

/*
    Small self-contained duplicates of the path-resolution / mmap-load
    helpers in aiquery.cpp. Kept local rather than shared across files in
    this plugin, matching this codebase's existing convention (see
    ifc_visualization/visualization.cpp) of not coupling plugin source
    files together for a handful of lines.
*/
namespace
{
    fs::path exe_dir() { return fs::canonical("/proc/self/exe").parent_path(); }
    fs::path server_root() { return exe_dir().parent_path(); }
    fs::path data_root() { return server_root().parent_path() / "storage"; }

    const std::string JSON_DIR = (data_root() / "outputs" / "gltf").string() + "/";
    const std::string SEMANTIC_EXT = ".json";

    std::string model_name_from_map_path(const std::string& model_path)
    {
        return fs::path(model_path).stem().stem().string();
    }

    std::string resolve_semantic_path_from_model(const std::string& model)
    {
        return JSON_DIR + model_name_from_map_path(model) + SEMANTIC_EXT;
    }

    bool load_json_mmap(const std::string& path, json& out)
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

        void* data = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);

        if (data == MAP_FAILED)
            return false;

        try
        {
            out = json::parse(
                static_cast<const char*>(data),
                static_cast<const char*>(data) + st.st_size);
        }
        catch (...)
        {
            munmap(data, st.st_size);
            return false;
        }

        munmap(data, st.st_size);
        return true;
    }

    std::string trim(const std::string& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string normalize_whitespace(const std::string& s)
    {
        std::string out;
        bool last_space = false;
        for (char c : s)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                if (!last_space)
                    out.push_back(' ');
                last_space = true;
            }
            else
            {
                out.push_back(c);
                last_space = false;
            }
        }
        return trim(out);
    }

    std::string strip_trailing_punct(const std::string& s)
    {
        std::string out = s;
        while (!out.empty() && (out.back() == '.' || out.back() == '?' || out.back() == '!'))
            out.pop_back();
        return out;
    }

    std::string to_lower(const std::string& s)
    {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        return out;
    }

    std::string title_case_word(const std::string& s)
    {
        std::string out = to_lower(s);
        if (!out.empty())
            out[0] = std::toupper(static_cast<unsigned char>(out[0]));
        return out;
    }

    // Naive English singularizer: strip one trailing 's'. Correctly handles
    // every category observed in real exported data, including mass nouns
    // that don't pluralize (e.g. "Furniture", "Casework" have no trailing
    // 's' to begin with, so they round-trip unchanged).
    std::string singularize_lower(const std::string& s)
    {
        std::string out = to_lower(trim(s));
        if (out.size() > 1 && out.back() == 's')
            out.pop_back();
        return out;
    }

    // ---- Per-model canonical category vocabulary, derived live from that
    // model's own semantic JSON and cached (no invalidation; a server
    // restart picks up re-exported data). ----

    std::mutex g_cache_mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_category_cache;

    std::unordered_map<std::string, std::string> build_category_table(const std::string& model)
    {
        std::unordered_map<std::string, std::string> table;

        json sem;
        if (!load_json_mmap(resolve_semantic_path_from_model(model), sem) || !sem.is_object())
            return table;

        for (const auto& bucket_entry : sem.items())
        {
            const json& bucket = bucket_entry.value();
            if (!bucket.is_object() || !bucket.contains("items") || !bucket["items"].is_array())
                continue;

            for (const auto& item : bucket["items"])
            {
                if (!item.is_object())
                    continue;

                auto it = item.find("category");
                if (it == item.end() || !it->is_string())
                    continue;

                std::string canonical = it->get<std::string>();
                table[singularize_lower(canonical)] = canonical;
            }
        }

        return table;
    }

    const std::unordered_map<std::string, std::string>& get_category_table(const std::string& model)
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_category_cache.find(model);
        if (it != g_category_cache.end())
            return it->second;

        auto [inserted_it, _] = g_category_cache.emplace(model, build_category_table(model));
        return inserted_it->second;
    }

    // ---- Grammar ----

    bool try_parse_clause(const std::string& clause, json& filters)
    {
        std::smatch cm;

        if (std::regex_match(clause, cm,
                              std::regex(R"(^(?:located\s+)?(?:on|in)\s+level\s+(.+)$)", std::regex::icase)))
        {
            filters.push_back({{"field", "Level"}, {"op", "eq"}, {"value", trim(cm[1].str())}});
            return true;
        }
        if (std::regex_match(clause, cm,
                              std::regex(R"(^whose\s+(\S+)\s+contains\s+(.+)$)", std::regex::icase)))
        {
            filters.push_back({{"field", title_case_word(cm[1].str())}, {"op", "contains"}, {"value", trim(cm[2].str())}});
            return true;
        }
        if (std::regex_match(clause, cm,
                              std::regex(R"(^manufactured\s+by\s+(.+)$)", std::regex::icase)))
        {
            filters.push_back({{"field", "Manufacturer"}, {"op", "eq"}, {"value", trim(cm[1].str())}});
            return true;
        }
        if (std::regex_match(clause, cm,
                              std::regex(R"(^named\s+(.+)$)", std::regex::icase)))
        {
            filters.push_back({{"field", "Name"}, {"op", "eq"}, {"value", trim(cm[1].str())}});
            return true;
        }

        static const std::vector<std::tuple<const char*, const char*, const char*>> dim_patterns = {
            {R"(^wider\s+than\s+([0-9]+(?:\.[0-9]+)?)\s*(?:mm|cm|m|in|inches|ft)?$)", "Width", "gt"},
            {R"(^narrower\s+than\s+([0-9]+(?:\.[0-9]+)?)\s*(?:mm|cm|m|in|inches|ft)?$)", "Width", "lt"},
            {R"(^taller\s+than\s+([0-9]+(?:\.[0-9]+)?)\s*(?:mm|cm|m|in|inches|ft)?$)", "Height", "gt"},
            {R"(^shorter\s+than\s+([0-9]+(?:\.[0-9]+)?)\s*(?:mm|cm|m|in|inches|ft)?$)", "Height", "lt"},
            {R"(^longer\s+than\s+([0-9]+(?:\.[0-9]+)?)\s*(?:mm|cm|m|in|inches|ft)?$)", "Length", "gt"},
        };

        for (const auto& [pattern, field, op] : dim_patterns)
        {
            if (std::regex_match(clause, cm, std::regex(pattern, std::regex::icase)))
            {
                filters.push_back({{"field", field}, {"op", op}, {"value", cm[1].str()}});
                return true;
            }
        }

        return false;
    }
}

bool try_parse_deterministic(const std::string& nl, const std::string& model, json& out_ast)
{
    std::string s = strip_trailing_punct(normalize_whitespace(nl));
    if (s.empty())
        return false;

    static const std::regex verb_re(
        R"(^(?:find|list|show|select|get|count)\s+(?:all|every|the)\s+(.+)$)", std::regex::icase);

    std::smatch m;
    if (!std::regex_match(s, m, verb_re))
        return false;

    std::string rest = m[1].str();

    static const std::vector<std::regex> boundary_res = {
        std::regex(R"(\blocated\s+on\s+level\b)", std::regex::icase),
        std::regex(R"(\blocated\s+in\s+level\b)", std::regex::icase),
        std::regex(R"(\bon\s+level\b)", std::regex::icase),
        std::regex(R"(\bin\s+level\b)", std::regex::icase),
        std::regex(R"(\bwhose\b)", std::regex::icase),
        std::regex(R"(\bmanufactured\s+by\b)", std::regex::icase),
        std::regex(R"(\bnamed\b)", std::regex::icase),
        std::regex(R"(\bwider\s+than\b)", std::regex::icase),
        std::regex(R"(\bnarrower\s+than\b)", std::regex::icase),
        std::regex(R"(\btaller\s+than\b)", std::regex::icase),
        std::regex(R"(\bshorter\s+than\b)", std::regex::icase),
        std::regex(R"(\blonger\s+than\b)", std::regex::icase),
    };

    size_t boundary_pos = std::string::npos;
    for (const auto& re : boundary_res)
    {
        std::smatch bm;
        if (std::regex_search(rest, bm, re))
            boundary_pos = std::min(boundary_pos, static_cast<size_t>(bm.position(0)));
    }

    std::string category_phrase = trim(boundary_pos == std::string::npos ? rest : rest.substr(0, boundary_pos));
    std::string clause_chain = boundary_pos == std::string::npos ? "" : trim(rest.substr(boundary_pos));

    if (category_phrase.empty())
        return false;

    const auto& category_table = get_category_table(model);
    if (category_table.empty())
        return false; // couldn't load this model's semantic data -> defer to LLM

    auto cat_it = category_table.find(singularize_lower(category_phrase));
    if (cat_it == category_table.end())
        return false; // category not recognized in this model's real data -> defer to LLM

    json filters = json::array();
    filters.push_back({{"field", "category"}, {"op", "eq"}, {"value", cat_it->second}});

    if (!clause_chain.empty())
    {
        static const std::regex and_re(R"(\s+and\s+)", std::regex::icase);
        std::sregex_token_iterator tok_it(clause_chain.begin(), clause_chain.end(), and_re, -1);
        std::sregex_token_iterator tok_end;

        for (; tok_it != tok_end; ++tok_it)
        {
            std::string clause = trim(tok_it->str());
            if (clause.empty())
                continue;
            if (!try_parse_clause(clause, filters))
                return false; // unrecognized clause -> not confident, defer to LLM
        }
    }

    out_ast = json{
        {"op", "semantic_filter"},
        {"match", "all"},
        {"filters", filters}};
    return true;
}
