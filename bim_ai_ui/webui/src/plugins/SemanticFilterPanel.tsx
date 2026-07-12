import React, { useState } from "react";
import api from "../api";
import { pickField } from "../components/semantic-inspector/fieldUtils";
import "./SemanticFilterPanel.css";

// TEMP-DEBUG(json-query-mode): this UI toggle and the JSON editor branch below
// exist only to validate the semantic filtering engine independently of NLP
// translation. Remove them, and the "Natural Language" / "JSON Query" mode
// state, once the semantic engine has been fully validated -- the NL query
// path below should then be the only path again. The backend "structured_query"
// action this mode calls is NOT temporary and should stay.
type QueryMode = "nl" | "json";

// Hidden temporarily per request; flip to true to bring the debug view back.
const SHOW_NLP_DEBUG = false;

export default function SemanticFilterPanel({ selectedModel }: { selectedModel: string }) {
  const [mode, setMode] = useState<QueryMode>("nl");
  const [query, setQuery] = useState("");
  const [jsonText, setJsonText] = useState("");
  const [response, setResponse] = useState<any>(null);
  const [requestError, setRequestError] = useState<string>("");
  const [loading, setLoading] = useState(false);
  const [expanded, setExpanded] = useState<Record<number, boolean>>({});

  function toggleCard(index: number) {
    setExpanded((prev) => ({ ...prev, [index]: !prev[index] }));
  }

  async function runQuery() {
    setRequestError("");
    setResponse(null);

    if (!selectedModel) {
      setRequestError("Please choose a model in the Viewer above.");
      return;
    }

    const model = selectedModel.replace(/\.glb$/i, ".map.json");
    let req: any;

    if (mode === "json") {
      if (!jsonText.trim()) {
        setRequestError("Please enter a query JSON.");
        return;
      }
      let ast: any;
      try {
        ast = JSON.parse(jsonText);
      } catch (e) {
        setRequestError("Invalid JSON.");
        return;
      }
      req = { action: "structured_query", model, ast };
    } else {
      if (!query.trim()) {
        setRequestError("Please enter a query.");
        return;
      }
      req = { action: "nl_query", model, nl: query };
    }

    setLoading(true);
    try {
      const res = await api.post("/aiquery", req);
      setResponse(res.data);
      setExpanded({});
    } catch (e) {
      setRequestError("Request failed.");
    } finally {
      setLoading(false);
    }
  }

  const backendError = response && response.error;
  const results = response && Array.isArray(response.results) ? response.results : null;

  return (
    <div className="sfp-page">
      <h3>Semantic Query</h3>

      <div className="sfp-section">
        <label className="sfp-label">Model</label>
        <div className="sfp-active-model">
          {selectedModel || "-- choose a model in the Viewer above --"}
        </div>
      </div>

      {/* TEMP-DEBUG(json-query-mode): remove this toggle once the semantic
          engine has been fully validated -- see note near QueryMode above. */}
      <div className="sfp-section">
        <label className="sfp-label">Mode</label>
        <div className="sfp-mode-toggle">
          <button
            className={mode === "nl" ? "sfp-mode-btn sfp-mode-btn-active" : "sfp-mode-btn"}
            onClick={() => setMode("nl")}
          >
            Natural Language
          </button>
          <button
            className={mode === "json" ? "sfp-mode-btn sfp-mode-btn-active" : "sfp-mode-btn"}
            onClick={() => setMode("json")}
          >
            JSON Query (Temporary Debug Mode)
          </button>
        </div>
      </div>
      {/* END TEMP-DEBUG(json-query-mode) */}

      {mode === "nl" ? (
        <div className="sfp-section">
          <label className="sfp-label">Query</label>
          <textarea
            className="sfp-query-input"
            placeholder="e.g. Find all doors on Level 1"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            rows={3}
          />
        </div>
      ) : (
        <div className="sfp-section">
          <label className="sfp-label">Query JSON (op / match / filters)</label>
          <textarea
            className="sfp-query-input sfp-json-input"
            placeholder={'{\n  "op": "semantic_filter",\n  "match": "all",\n  "filters": [\n    { "field": "category", "op": "eq", "value": "Doors" }\n  ]\n}'}
            value={jsonText}
            onChange={(e) => setJsonText(e.target.value)}
            rows={10}
          />
        </div>
      )}

      <div className="sfp-section">
        <button className="sfp-run-btn" onClick={runQuery} disabled={loading}>
          {loading ? "Running..." : "Run Query"}
        </button>
      </div>

      {requestError && <div className="sfp-error">{requestError}</div>}

      {backendError && (
        <div className="sfp-error">
          {String(response.error)}
          {response.path ? ` (path: ${response.path})` : ""}
        </div>
      )}

      {/* TEMP-DEBUG(nlp-validation): remove this block, and the matching backend field
          (debug_nlp_translation) in aiquery.cpp, once the NLP layer has been validated.
          Currently hidden via SHOW_NLP_DEBUG -- flip that back to true to re-show it. */}
      {SHOW_NLP_DEBUG && response && response.debug_nlp_translation && (
        <div className="sfp-section">
          <label className="sfp-label">NLP Translation (Temporary Debug View)</label>
          <pre className="sfp-json sfp-debug-json">
            {JSON.stringify(response.debug_nlp_translation, null, 2)}
          </pre>
        </div>
      )}
      {/* END TEMP-DEBUG(nlp-validation) */}

      {/* TEMP-DEBUG(step4-paths): remove or redesign once Step 4 (relationship
          path preservation) has been validated. Renders response.paths exactly
          as returned by the backend, no interpretation. */}
      {response && response.paths && (
        <div className="sfp-section">
          <label className="sfp-label">Paths (Temporary Debug View)</label>
          <pre className="sfp-json sfp-debug-json">
            {JSON.stringify(response.paths, null, 2)}
          </pre>
        </div>
      )}
      {/* END TEMP-DEBUG(step4-paths) */}

      {results && (
        <div className="sfp-section">
          <div className="sfp-count">
            {typeof response.count === "number" ? response.count : results.length} matches
          </div>
          <div className="sfp-results">
            {results.map((item: any, i: number) => {
              const category = pickField(item, ["category", "Category"]) ?? "—";
              const uniqueId = pickField(item, ["uniqueId", "UniqueId"]) ?? "—";
              const isOpen = !!expanded[i];
              return (
                <div className="sfp-card" key={i}>
                  <button className="sfp-card-header" onClick={() => toggleCard(i)}>
                    <span className="sfp-card-toggle">{isOpen ? "▾" : "▸"}</span>
                    <span className="sfp-card-title">
                      {String(category)} / {String(uniqueId)}
                    </span>
                  </button>
                  {isOpen && (
                    <pre className="sfp-json">{JSON.stringify(item, null, 2)}</pre>
                  )}
                </div>
              );
            })}
          </div>
        </div>
      )}
    </div>
  );
}
