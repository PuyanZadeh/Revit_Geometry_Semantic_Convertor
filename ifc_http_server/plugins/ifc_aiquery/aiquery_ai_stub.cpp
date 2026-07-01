#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

using json = nlohmann::json;

/*
    FINAL AI HOOK (stub)
    • Input: natural language string (the text prompt)
    • Output: AST conforming to aiquery_ast_contract.json
    • This file is the ONLY future replacement point
    • No other code needs to change, ever
*/


json ai_nl_to_ast(const std::string& nl_raw)
{
    // FINAL PLACEHOLDER
    // The text prompt is already in `nl`
    // A real AI implementation will replace ONLY this body

    std::string nl = nl_raw;
    std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);

    json ast = json::object();
    ast["filters"] = json::array();

    /* ---------------- count ---------------- */
    if (nl.find("count") != std::string::npos)
    {
        ast["op"] = "count_by_type";

        if (nl.find("window") != std::string::npos)
            ast["type"] = "IFCWINDOW";
        else if (nl.find("door") != std::string::npos)
            ast["type"] = "IFCDOOR";
        else
            ast["type"] = "IFCUNKNOWN";

        /* ---------------- level filter ---------------- */
        auto pos = nl.find("level");
        if (pos != std::string::npos)
        {
            std::string lvl;
            for (size_t i = pos + 5; i < nl.size(); ++i)
            {
                if (std::isdigit(nl[i]))
                    lvl.push_back(nl[i]);
                else if (!lvl.empty())
                    break;
            }
            if (!lvl.empty())
            {
                ast["filters"].push_back({
                    {"field","Name"},
                    {"op","eq"},
                    {"value", lvl}
                });
            }
        }

        return ast;
    }

    /* ---------------- fallback ---------------- */
    ast["op"] = "unknown";
    ast["raw"] = nl_raw;
    return ast;
}
