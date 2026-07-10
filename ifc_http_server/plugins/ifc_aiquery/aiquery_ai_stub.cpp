#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <cstdlib>
#include <string>
#include <sstream>
#include <iostream>

#include "nl_rule_parser.h"

using json = nlohmann::json;

/*
    NL -> AST translation.

    ai_nl_to_ast() tries the deterministic rule-based parser (nl_rule_parser)
    first. It is only called out to the LLM below when the rule parser
    cannot confidently translate the query. Performance, determinism, and
    predictability take priority over NLP flexibility — the LLM is a
    fallback, not the primary parser.
*/

namespace
{
    const char* kModel = "claude-haiku-4-5";
    const char* kApiUrl = "https://api.anthropic.com/v1/messages";

    const char* kSystemPrompt =
        "You translate a natural-language question about a building information model (BIM/IFC) "
        "into a small structured query for a generic semantic filtering engine. "
        "You do not know the traversal or filtering implementation; you only produce the abstract "
        "query representation described below. There is no separate counting operation — the "
        "filtering engine reports how many elements matched, so a request to count/list/find "
        "elements is handled the same way.\n\n"
        "Output fields:\n"
        "- op: \"semantic_filter\" for any request that asks to find, list, select, or count "
        "elements matching some criteria (category, level, name, manufacturer, dimensions, any "
        "attribute). Use \"unknown\" only if the request does not describe a queryable element "
        "search at all.\n"
        "- match: \"all\" (AND) or \"any\" (OR) across filters. Default \"all\".\n"
        "- filters: a list of {field, op, value}. This is the ONLY place criteria go — there is no "
        "other field for expressing what to match. field is any attribute name mentioned or "
        "implied by the request (e.g. category, Level, Name, Manufacturer, Width) — do not invent "
        "a fixed vocabulary, use the term that best matches what the user said. op is one of: eq, "
        "neq, contains, gt, gte, lt, lte, exists. value is a plain string; for numeric comparisons "
        "put just the number (e.g. \"1200\"), no units.\n"
        "- Every request that names an element category (doors, walls, windows, rooms, furniture, "
        "...) MUST include a filter on field \"category\" — never omit it. Source data category "
        "values are typically plural (e.g. \"Doors\", \"Windows\", \"Rooms\"), so use op "
        "\"contains\" with the singular root word capitalized (e.g. \"Door\", \"Wall\", \"Room\", "
        "\"Furniture\", \"Window\") so it matches regardless of exact pluralization or wording.\n\n"
        "Examples of intent (for calibration only, not literal strings to match):\n"
        "\"Find all doors\" -> op: semantic_filter, filters: [{field: category, op: contains, "
        "value: Door}]\n"
        "\"Count all windows\" -> op: semantic_filter, filters: [{field: category, op: contains, "
        "value: Window}]\n"
        "\"Find all doors on Level 1\" -> op: semantic_filter, filters: [{field: category, "
        "op: contains, value: Door}, {field: Level, op: eq, value: 1}], match: all\n"
        "\"Find all rooms whose Name contains Office\" -> op: semantic_filter, filters: "
        "[{field: category, op: contains, value: Room}, {field: Name, op: contains, value: "
        "Office}]\n"
        "\"Find all windows wider than 1200 mm\" -> op: semantic_filter, filters: [{field: "
        "category, op: contains, value: Window}, {field: Width, op: gt, value: 1200}]\n\n"
        "Return only the structured output. Never explain your reasoning.";

    json output_schema()
    {
        return json{
            {"type", "object"},
            {"properties", {
                {"op", {
                    {"type", "string"},
                    {"enum", {"semantic_filter", "unknown"}}
                }},
                {"match", {
                    {"type", "string"},
                    {"enum", {"all", "any"}}
                }},
                {"filters", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"field", {{"type", "string"}}},
                            {"op", {
                                {"type", "string"},
                                {"enum", {"eq", "neq", "contains", "gt", "gte", "lt", "lte", "exists"}}
                            }},
                            {"value", {{"type", "string"}}}
                        }},
                        {"required", {"field", "op"}},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", {"op", "filters"}},
            {"additionalProperties", false}
        };
    }

    size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* out = static_cast<std::string*>(userdata);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    }

    json fallback(const std::string& nl_raw)
    {
        return json{{"op", "unknown"}, {"raw", nl_raw}};
    }
}

namespace
{
    json ai_nl_to_ast_llm(const std::string& nl_raw)
    {
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key || !*api_key)
    {
        std::cerr << "[aiquery_ai_stub] ANTHROPIC_API_KEY not set; returning unknown op" << std::endl;
        return fallback(nl_raw);
    }

    json request_body = {
        {"model", kModel},
        {"max_tokens", 1024},
        {"system", kSystemPrompt},
        {"output_config", {
            {"format", {
                {"type", "json_schema"},
                {"schema", output_schema()}
            }}
        }},
        {"messages", json::array({
            {{"role", "user"}, {"content", nl_raw}}
        })}
    };

    std::string request_str = request_body.dump();
    std::string response_str;

    CURL* curl = curl_easy_init();
    if (!curl)
        return fallback(nl_raw);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    std::string api_key_header = std::string("x-api-key: ") + api_key;
    headers = curl_slist_append(headers, api_key_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, kApiUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        std::cerr << "[aiquery_ai_stub] curl error: " << curl_easy_strerror(res) << std::endl;
        return fallback(nl_raw);
    }

    if (http_status < 200 || http_status >= 300)
    {
        std::cerr << "[aiquery_ai_stub] Claude API HTTP " << http_status << ": " << response_str << std::endl;
        return fallback(nl_raw);
    }

    try
    {
        json response = json::parse(response_str);

        if (response.value("stop_reason", "") == "refusal")
        {
            std::cerr << "[aiquery_ai_stub] Claude API refused the request" << std::endl;
            return fallback(nl_raw);
        }

        for (const auto& block : response.value("content", json::array()))
        {
            if (block.value("type", "") == "text")
            {
                json ast = json::parse(block.value("text", ""));
                if (!ast.contains("filters"))
                    ast["filters"] = json::array();
                return ast;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[aiquery_ai_stub] failed to parse Claude response: " << e.what() << std::endl;
    }

    return fallback(nl_raw);
    }
}

json ai_nl_to_ast(const std::string& nl_raw, const std::string& model)
{
    json ast;
    if (try_parse_deterministic(nl_raw, model, ast))
    {
        ast["source"] = "rule_based";
        return ast;
    }

    ast = ai_nl_to_ast_llm(nl_raw);
    ast["source"] = "llm";
    return ast;
}
