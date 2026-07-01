// aiquery_ai_stub.h
#pragma once
#include <string>
#include <nlohmann/json.hpp>

/*
    AI NL → AST interface
    • Stable forever
    • Implementation can change (stub → real AI)
*/

nlohmann::json ai_nl_to_ast(const std::string& nl);
