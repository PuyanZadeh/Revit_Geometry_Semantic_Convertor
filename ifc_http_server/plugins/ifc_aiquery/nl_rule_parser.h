// nl_rule_parser.h
#pragma once
#include <string>
#include <nlohmann/json.hpp>

/*
    Deterministic, rule-based NL -> AST translator.

    Tries to translate `nl` into the same AST shape ai_nl_to_ast() produces
    (op/match/filters), using explicit grammar templates and a live,
    per-model canonical category vocabulary derived from that model's own
    semantic JSON (not a hardcoded guess).

    Returns true only when the ENTIRE input was confidently understood
    (recognized verb + category, plus zero or more fully-recognized
    clauses, with nothing left over). On false, the caller should fall
    back to the LLM — this function never returns a partial/best-guess AST.
*/
bool try_parse_deterministic(const std::string& nl, const std::string& model, nlohmann::json& out_ast);
