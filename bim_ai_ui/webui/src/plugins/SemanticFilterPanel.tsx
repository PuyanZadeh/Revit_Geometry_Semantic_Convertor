import React, { useEffect, useState } from "react";
import api from "../api";
import { pickField } from "../components/semantic-inspector/fieldUtils";
import "./SemanticFilterPanel.css";

const OPERATORS = ["eq", "neq", "contains", "gt", "gte", "lt", "lte", "exists"];

interface FilterRow {
  field: string;
  op: string;
  value: string;
}

function emptyRow(): FilterRow {
  return { field: "", op: "eq", value: "" };
}

export default function SemanticFilterPanel() {
  const [files, setFiles] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>("");
  const [rows, setRows] = useState<FilterRow[]>([emptyRow()]);
  const [match, setMatch] = useState<"all" | "any">("all");
  const [response, setResponse] = useState<any>(null);
  const [requestError, setRequestError] = useState<string>("");
  const [loading, setLoading] = useState(false);
  const [expanded, setExpanded] = useState<Record<number, boolean>>({});

  useEffect(() => {
    async function loadList() {
      const req = { action: "list_models" };
      const res = await api.post("/aiquery", req);
      const sorted = (res.data.models || []).sort();
      setFiles(sorted);
    }
    loadList();
  }, []);

  function updateRow(index: number, patch: Partial<FilterRow>) {
    setRows((prev) => prev.map((r, i) => (i === index ? { ...r, ...patch } : r)));
  }

  function addRow() {
    setRows((prev) => [...prev, emptyRow()]);
  }

  function removeRow(index: number) {
    setRows((prev) => prev.filter((_, i) => i !== index));
  }

  function toggleCard(index: number) {
    setExpanded((prev) => ({ ...prev, [index]: !prev[index] }));
  }

  async function runFilter() {
    setRequestError("");
    setResponse(null);

    if (!selected) {
      setRequestError("Please choose a model.");
      return;
    }

    const filters = rows
      .filter((r) => r.field.trim() !== "")
      .map((r) => (r.op === "exists" ? { field: r.field, op: r.op } : { field: r.field, op: r.op, value: r.value }));

    const req = {
      action: "semantic_filter",
      model: selected,
      match,
      filters,
    };

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
      <h3>Step 1 — Semantic Filter</h3>

      <div className="sfp-section">
        <label className="sfp-label">Model</label>
        <select
          value={selected}
          onChange={(e) => setSelected(e.target.value)}
          className="sfp-select"
        >
          <option value="">-- choose model --</option>
          {files.map((f) => (
            <option key={f} value={f}>
              {f}
            </option>
          ))}
        </select>
      </div>

      <div className="sfp-section">
        <label className="sfp-label">Filters</label>
        <div className="sfp-filter-rows">
          {rows.map((row, i) => (
            <div className="sfp-filter-row" key={i}>
              <input
                className="sfp-input sfp-field"
                placeholder="field (e.g. category, hostId, Level)"
                value={row.field}
                onChange={(e) => updateRow(i, { field: e.target.value })}
              />
              <select
                className="sfp-select sfp-op"
                value={row.op}
                onChange={(e) => updateRow(i, { op: e.target.value })}
              >
                {OPERATORS.map((op) => (
                  <option key={op} value={op}>
                    {op}
                  </option>
                ))}
              </select>
              <input
                className="sfp-input sfp-value"
                placeholder="value"
                value={row.value}
                disabled={row.op === "exists"}
                onChange={(e) => updateRow(i, { value: e.target.value })}
              />
              <button
                className="sfp-remove-btn"
                onClick={() => removeRow(i)}
                disabled={rows.length === 1}
                title="Remove filter"
              >
                ✕
              </button>
            </div>
          ))}
        </div>
        <button className="sfp-add-btn" onClick={addRow}>
          + Add Filter
        </button>
      </div>

      <div className="sfp-section">
        <label className="sfp-label">Match mode</label>
        <div className="sfp-radio-group">
          <label className="sfp-radio">
            <input
              type="radio"
              name="match-mode"
              checked={match === "all"}
              onChange={() => setMatch("all")}
            />
            AND
          </label>
          <label className="sfp-radio">
            <input
              type="radio"
              name="match-mode"
              checked={match === "any"}
              onChange={() => setMatch("any")}
            />
            OR
          </label>
        </div>
      </div>

      <div className="sfp-section">
        <button className="sfp-run-btn" onClick={runFilter} disabled={loading}>
          {loading ? "Running..." : "Run"}
        </button>
      </div>

      {requestError && <div className="sfp-error">{requestError}</div>}

      {backendError && (
        <div className="sfp-error">
          {String(response.error)}
          {response.path ? ` (path: ${response.path})` : ""}
        </div>
      )}

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
