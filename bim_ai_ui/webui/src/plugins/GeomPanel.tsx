import React, { useState } from "react";
import { runGeom } from "../api/geom";
import "./SemanticFilterPanel.css";

// Dedicated debugging/QC interface for the geometry engine (Phase 2, Layer
// 1). Deliberately raw and transparent, not polished: the textarea's exact
// contents are POSTed to /geom verbatim (via runGeom -> api.post("/geom",
// payload)) on Run, with no field injected, merged, or defaulted
// automatically -- the selected model and picked element are shown only as
// read-only references (reusing the existing Viewer selection state). The
// only way either value reaches the JSON is the explicit "Insert into JSON"
// button beside the picked element, which deterministically edits the
// textarea's own text on click -- never automatically, never on Run. The
// response is rendered as raw JSON with no interpretation, so backend
// errors surface exactly as returned. This panel never calls /aiquery -- it
// is independent of the semantic engine.
export default function GeomPanel({
  selectedModel,
  selectedElementId,
}: {
  selectedModel: string;
  selectedElementId: string;
}) {
  const [jsonText, setJsonText] = useState("");
  const [response, setResponse] = useState<any>(null);
  const [requestError, setRequestError] = useState<string>("");
  const [loading, setLoading] = useState(false);

  // Parses the current textarea content into an editable object, or "{}" if
  // empty. Shared by both insert actions below; never invented/defaulted --
  // only ever runs on explicit button click, never automatically.
  function parseForInsert(): any | undefined {
    if (!jsonText.trim()) return {};

    let parsed: any;
    try {
      parsed = JSON.parse(jsonText);
    } catch (e: any) {
      setRequestError("Cannot insert: current JSON is invalid (" + (e?.message || String(e)) + ")");
      return undefined;
    }
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      setRequestError("Cannot insert: request JSON must be an object.");
      return undefined;
    }
    return parsed;
  }

  // Deterministically edits the textarea's own text to include the picked
  // element in its elementIds array -- only ever runs on explicit click,
  // never automatically. Creates the array if missing; appends the picked
  // ID only if not already present; leaves everything else untouched.
  function insertPickedElement() {
    setRequestError("");
    if (!selectedElementId) return;

    const parsed = parseForInsert();
    if (parsed === undefined) return;

    if (!Array.isArray(parsed.elementIds)) {
      parsed.elementIds = [];
    }
    if (!parsed.elementIds.includes(selectedElementId)) {
      parsed.elementIds.push(selectedElementId);
    }

    setJsonText(JSON.stringify(parsed, null, 2));
  }

  // Deterministically edits the textarea's own text to set the "model" key
  // to the currently selected model -- only that key, nothing else touched.
  // Same discipline as insertPickedElement: explicit click only.
  function insertModel() {
    setRequestError("");
    if (!selectedModel) return;

    const parsed = parseForInsert();
    if (parsed === undefined) return;

    parsed.model = selectedModel;

    setJsonText(JSON.stringify(parsed, null, 2));
  }

  async function runQuery() {
    setRequestError("");
    setResponse(null);

    if (!jsonText.trim()) {
      setRequestError("Please enter a request JSON.");
      return;
    }

    let payload: any;
    try {
      payload = JSON.parse(jsonText);
    } catch (e: any) {
      setRequestError("Invalid JSON: " + (e?.message || String(e)));
      return;
    }

    // Application state, not part of the query itself: inject the Viewer's
    // selected model only when the request doesn't already specify one --
    // never override an explicit value.
    if (payload.model === undefined && selectedModel) {
      payload.model = selectedModel;
    }

    setLoading(true);
    try {
      const data = await runGeom(payload);
      setResponse(data);
    } catch (e: any) {
      // Surface the backend's own response body when available (e.g. a
      // routing-level 404 "plugin not found"), otherwise the raw axios
      // error -- either way, nothing invented or summarized.
      if (e?.response?.data !== undefined) {
        setResponse(e.response.data);
      } else {
        setRequestError("Request failed: " + (e?.message || String(e)));
      }
    } finally {
      setLoading(false);
    }
  }

  return (
    <div className="sfp-page">
      <h3>Geometry Query (Debug / QC)</h3>

      <div className="sfp-section">
        <label className="sfp-label">Model (reference only -- not auto-inserted)</label>
        <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
          <div className="sfp-active-model" style={{ flex: 1 }}>
            {selectedModel || "-- choose a model in the Viewer above --"}
          </div>
          <button
            className="sfp-run-btn"
            onClick={insertModel}
            disabled={!selectedModel}
            title="Sets the request JSON's model key to the currently selected model. Does not otherwise change the JSON."
          >
            Insert into JSON
          </button>
        </div>
      </div>

      <div className="sfp-section">
        <label className="sfp-label">Picked Element (reference only -- not auto-inserted)</label>
        <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
          <div className="sfp-active-model" style={{ flex: 1 }}>
            {selectedElementId || "-- click an element in the Viewer above --"}
          </div>
          <button
            className="sfp-run-btn"
            onClick={insertPickedElement}
            disabled={!selectedElementId}
            title="Adds this element's ID into the request JSON's elementIds array (creates the array if missing, skips it if already present). Does not otherwise change the JSON."
          >
            Insert into JSON
          </button>
        </div>
      </div>

      <div className="sfp-section">
        <label className="sfp-label">Request JSON (sent to /geom exactly as written)</label>
        <textarea
          className="sfp-query-input sfp-json-input"
          placeholder={'{\n  "action": "get_geometry",\n  "model": "Snowdon Towers Sample Architectural.light.glb",\n  "elementIds": ["<uniqueId>"],\n  "primitives": ["vertices", "faces", "meshEdges", "surfaceCentroid", "extentsCenter", "lowestPoint", "highestPoint"]\n}'}
          value={jsonText}
          onChange={(e) => setJsonText(e.target.value)}
          rows={12}
        />
      </div>

      <div className="sfp-section">
        <button className="sfp-run-btn" onClick={runQuery} disabled={loading}>
          {loading ? "Running..." : "Run"}
        </button>
      </div>

      {requestError && <div className="sfp-error">{requestError}</div>}

      {response !== null && (
        <div className="sfp-section">
          <label className="sfp-label">Raw Response</label>
          <pre className="sfp-json">{JSON.stringify(response, null, 2)}</pre>
        </div>
      )}
    </div>
  );
}
