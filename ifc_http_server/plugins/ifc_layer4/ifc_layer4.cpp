// Phase 2, Layer 4: workflow orchestration.
//
// Layer 4 is an independent component sitting between Phase 1 (the semantic
// engine, ifc_aiquery) and the geometry engine (Layers 1-3, ifc_geom). It
// calls both ONLY through their existing public HTTP interfaces (POST
// /aiquery, POST /geom) -- exactly as any external caller would -- never by
// linking against or calling their code directly. This is what keeps both
// engines completely independent while still allowing them to be combined:
// Layer 4 is the one recognized place that composition is allowed to
// happen, so neither engine ever needs a shortcut into the other.
//
// This file implements a GENERIC workflow executor, not any specific
// engineering workflow. A "workflow" is data supplied in the request itself
// (a caller-supplied set of named anchor entities plus an ordered list of
// steps) -- there is no hardcoded recipe (e.g. "sill height") anywhere in
// this file. See project memory for the full architectural derivation this
// implements; the short version of every rule enforced below:
//
//   - Every step names exactly one target ("phase1" or "geometry") and one
//     request body to send it. No step decides at runtime how to obtain a
//     value -- the caller's step list is the only source of sequencing.
//   - A step's request may reference an anchor ($anchors.<name>) or a prior
//     step's extracted output ($steps.<name>) -- substitution only, no
//     computation, no inference.
//   - Every step is checked for two independent things: did the call itself
//     fail (transport failure, non-2xx, unparseable body, or the
//     established convention that error responses carry a top-level
//     "error" field), and -- only if an "expect" is declared -- did the
//     result satisfy the declared cardinality. These are reported as
//     different failure reasons ("call_failed" vs "expectation_violated")
//     because they point at different things to go fix.
//   - The moment either check fails, execution stops immediately. Nothing
//     is guessed, no first-candidate is picked, no ambiguity is hidden --
//     the full set of candidates (when the checked path was an array) is
//     returned, not just a count, so the caller can see exactly what Phase
//     1 or the geometry engine actually returned.
//   - A completed run returns the full trace (every step's target, the
//     exact request sent, and the exact response received) so every
//     returned value is traceable to a specific lower-layer call. A halted
//     run returns the same trace for whatever completed, plus the failing
//     step's own request/response and the specific violation.
//
// Deliberately absent from this file: any compliance evaluation (pass/fail
// against a rule), any NLP/rationale logic, any hardcoded workflow. Those
// are separate, later components in the long-term architecture; Layer 4
// only orchestrates and reports.
#include "ifc_layer4.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    // Loopback base URL -- this server has no existing "call myself" or base
    // URL convention anywhere (confirmed: only main.cpp hardcodes port 8080
    // for its own listener). Introduced here for the first time, matching
    // this codebase's existing no-config-abstraction style.
    const char *kBaseUrl = "http://localhost:8080";

    bool resolve_target_url(const std::string &target, std::string &url)
    {
        if (target == "phase1")
        {
            url = std::string(kBaseUrl) + "/aiquery";
            return true;
        }
        if (target == "geometry")
        {
            url = std::string(kBaseUrl) + "/geom";
            return true;
        }
        return false;
    }

    size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *out = static_cast<std::string *>(userdata);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    }

    struct CallResult
    {
        bool transport_ok = false;
        long http_status = 0;
        std::string body;
    };

    // Mirrors the exact libcurl usage already established in
    // aiquery_ai_stub.cpp (that one calls an external API; this one calls
    // this same server's other two plugins) -- same pattern, not a new
    // HTTP-client convention.
    CallResult post_json(const std::string &url, const std::string &body_str)
    {
        CallResult result;

        CURL *curl = curl_easy_init();
        if (!curl)
            return result;

        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "content-type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);
        result.transport_ok = (res == CURLE_OK);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return result;
    }

    // Walks a JSON value by a dotted path (e.g. "results.0.uniqueId"); a
    // purely-numeric token is an array index, anything else is an object
    // key. Deliberately this simple -- no wildcards, no filtering, no
    // expressions. This is the smallest language that satisfies today's
    // need (pull one field, possibly through one array index); richer path
    // expressions are a future extension once a real workflow demands one,
    // not something to anticipate now.
    bool walk_path(const json &root, const std::string &path, json &out)
    {
        if (path.empty())
        {
            out = root;
            return true;
        }

        const json *cur = &root;
        size_t start = 0;

        while (start <= path.size())
        {
            size_t dot = path.find('.', start);
            std::string token = (dot == std::string::npos) ? path.substr(start) : path.substr(start, dot - start);

            if (token.empty())
                return false;

            bool all_digits = std::all_of(token.begin(), token.end(),
                                           [](unsigned char c)
                                           { return std::isdigit(c); });

            if (all_digits)
            {
                size_t idx = static_cast<size_t>(std::stoul(token));
                if (!cur->is_array() || idx >= cur->size())
                    return false;
                cur = &(*cur)[idx];
            }
            else
            {
                if (!cur->is_object() || !cur->contains(token))
                    return false;
                cur = &(*cur)[token];
            }

            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }

        out = *cur;
        return true;
    }

    // A string value of the exact form "$anchors.<name>" or "$steps.<name>"
    // is a placeholder; anything else is a literal string, left untouched.
    // Substitution only -- this never computes, combines, or infers a
    // value, it only looks one up by exact name.
    bool resolve_placeholder(const std::string &s, const json &anchors,
                              const std::unordered_map<std::string, json> &step_outputs, json &out)
    {
        const std::string anchor_prefix = "$anchors.";
        const std::string step_prefix = "$steps.";

        if (s.rfind(anchor_prefix, 0) == 0)
        {
            std::string name = s.substr(anchor_prefix.size());
            if (!anchors.contains(name))
                return false;
            out = anchors.at(name);
            return true;
        }

        if (s.rfind(step_prefix, 0) == 0)
        {
            std::string name = s.substr(step_prefix.size());
            auto it = step_outputs.find(name);
            if (it == step_outputs.end())
                return false;
            out = it->second;
            return true;
        }

        return false;
    }

    // Builds a new JSON value with every placeholder string replaced --
    // never mutates the step's original template, so the same step
    // definition could in principle be re-resolved deterministically given
    // the same anchors/step_outputs.
    json substitute(const json &node, const json &anchors, const std::unordered_map<std::string, json> &step_outputs)
    {
        if (node.is_string())
        {
            json resolved;
            if (resolve_placeholder(node.get<std::string>(), anchors, step_outputs, resolved))
                return resolved;
            return node;
        }
        if (node.is_object())
        {
            json out = json::object();
            for (const auto &item : node.items())
                out[item.key()] = substitute(item.value(), anchors, step_outputs);
            return out;
        }
        if (node.is_array())
        {
            json out = json::array();
            for (const auto &el : node)
                out.push_back(substitute(el, anchors, step_outputs));
            return out;
        }
        return node;
    }
}

extern "C"
{
    const char *plugin_name()
    {
        return "Layer4Orchestrator Plugin";
    }

    const char *handle_ifc_layer4(const std::string &input_json)
    {
        static std::string response;

        try
        {
            json req = json::parse(input_json);
            std::string action = req.value("action", "");

            if (action != "run_workflow")
            {
                response = json({{"plugin", "Layer4Orchestrator"}, {"status", "idle"}}).dump();
                return response.c_str();
            }

            json anchors = req.value("anchors", json::object());

            if (!req.contains("steps") || !req["steps"].is_array() || req["steps"].empty())
            {
                response = json({{"error", "missing or empty steps"}}).dump();
                return response.c_str();
            }

            std::unordered_map<std::string, json> step_outputs;
            json trace = json::array();

            for (const auto &step_def : req["steps"])
            {
                std::string step_name = step_def.value("name", "");
                std::string target = step_def.value("target", "");
                json request_template = step_def.value("request", json::object());

                std::string url;
                if (step_name.empty() || target.empty() || !resolve_target_url(target, url))
                {
                    response = json({{"status", "halted"},
                                     {"trace", trace},
                                     {"failedStep", step_name},
                                     {"reason", "invalid_step_definition"},
                                     {"detail", "step must have a non-empty \"name\" and a \"target\" of "
                                                "\"phase1\" or \"geometry\""}})
                                   .dump();
                    return response.c_str();
                }

                json resolved_request = substitute(request_template, anchors, step_outputs);
                CallResult call = post_json(url, resolved_request.dump());

                json parsed_response;
                bool parsed_ok = false;
                if (call.transport_ok && call.http_status >= 200 && call.http_status < 300)
                {
                    try
                    {
                        parsed_response = json::parse(call.body);
                        parsed_ok = true;
                    }
                    catch (...)
                    {
                        parsed_ok = false;
                    }
                }

                // Call-failure check -- distinct from expectation violation
                // below. Covers transport failure, non-2xx status,
                // unparseable body, and the established convention that
                // every error response in this system carries a top-level
                // "error" field.
                if (!parsed_ok || parsed_response.contains("error"))
                {
                    response = json({{"status", "halted"},
                                     {"trace", trace},
                                     {"failedStep", step_name},
                                     {"target", target},
                                     {"reason", "call_failed"},
                                     {"request", resolved_request},
                                     {"response", parsed_ok ? parsed_response : json(call.body)},
                                     {"detail", parsed_ok
                                                    ? parsed_response.value("error", "")
                                                    : ("transport failure or non-JSON response (http status " +
                                                       std::to_string(call.http_status) + ")")}})
                                   .dump();
                    return response.c_str();
                }

                // Expectation check -- optional per step, cardinality only
                // (the smallest language that satisfies today's need). A
                // step with no "expect" only requires the call to have
                // succeeded, checked above.
                if (step_def.contains("expect") && step_def["expect"].is_object())
                {
                    std::string path = step_def["expect"].value("path", "");
                    int expected_count = step_def["expect"].value("count", -1);

                    json found;
                    bool path_ok = walk_path(parsed_response, path, found);

                    bool satisfied = path_ok && found.is_array() &&
                                      expected_count >= 0 &&
                                      static_cast<int>(found.size()) == expected_count;

                    if (!satisfied)
                    {
                        response = json({{"status", "halted"},
                                         {"trace", trace},
                                         {"failedStep", step_name},
                                         {"target", target},
                                         {"reason", "expectation_violated"},
                                         {"request", resolved_request},
                                         {"response", parsed_response},
                                         {"expected", step_def["expect"]},
                                         {"actual", path_ok ? found : json(nullptr)},
                                         {"candidates", (path_ok && found.is_array()) ? found : json::array()}})
                                       .dump();
                        return response.c_str();
                    }
                }

                // Extraction -- pulls a named value out of this step's
                // response for later steps to reference via
                // $steps.<name>. Independent of "expect": a step can
                // extract without declaring an expectation, and vice versa.
                if (step_def.contains("extract") && step_def["extract"].is_string())
                {
                    json extracted;
                    if (walk_path(parsed_response, step_def["extract"].get<std::string>(), extracted))
                        step_outputs[step_name] = extracted;
                }

                trace.push_back(json({{"step", step_name},
                                      {"target", target},
                                      {"request", resolved_request},
                                      {"response", parsed_response}}));
            }

            response = json({{"status", "completed"}, {"trace", trace}}).dump();
            return response.c_str();
        }
        catch (const std::exception &e)
        {
            response = json({{"error", std::string("invalid input or runtime error: ") + e.what()}}).dump();
            return response.c_str();
        }
        catch (...)
        {
            response = R"({"error":"invalid input or runtime error"})";
            return response.c_str();
        }
    }
}
