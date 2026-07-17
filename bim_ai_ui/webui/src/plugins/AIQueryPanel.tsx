import React, { useEffect, useState } from "react";
import api from "../api";

export default function AIQueryPanel() {
  const [files, setFiles] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>("");
  const [result, setResult] = useState<string>("");
  const [query, setQuery] = useState<string>("3kTGy1iIsrNeOJJlhD9vy_");
//   const [query, setQuery] = useState<string>("");
  
  useEffect(() => {
    async function loadList() {
      const req = { action: "list_models" };
      const res = await api.post("/aiquery", req);
      const sorted = (res.data.models || []).sort();
      setFiles(sorted);
    }
    loadList();
  }, []);

  /* async function runQuery() {
    if (!selected || !query) return;

    const req = {
      action: "validate_id",
    //   action: "query_model",
      model: selected,
    //   query: query
      globalId: query
    };

    const res = await api.post("/aiquery", req);
    setResult(JSON.stringify(res.data, null, 2));
  } */
 async function runQuery() {
  if (!query) return;

  let payload: any;
  try {
    payload = JSON.parse(query);
  } catch {
    payload = undefined;
  }

  if (payload !== undefined && typeof payload === "object" && payload !== null && !Array.isArray(payload)) {
    // Raw JSON mode -- sent to /aiquery exactly as written, same convention
    // GeomPanel.tsx uses for /geom. Only fills in `model` when the JSON
    // doesn't already specify one; never overrides an explicit value.
    if (payload.model === undefined && selected) {
      payload.model = selected;
    }
  } else {
    // Not valid JSON (or not a plain object) -- existing natural-language
    // workflow, unchanged.
    if (!selected) return;
    payload = {
      action: "nl_query",
      model: selected,
      nl: query
    };
  }

  try {
    const res = await api.post("/aiquery", payload);
    setResult(JSON.stringify(res.data, null, 2));
  } catch (e: any) {
    if (e?.response?.data !== undefined) {
      setResult(JSON.stringify(e.response.data, null, 2));
    } else {
      setResult("Request failed: " + (e?.message || String(e)));
    }
  }
}


  function handleSelect(e: React.ChangeEvent<HTMLSelectElement>) {
    setSelected(e.target.value);
    setResult("");
  }

  return (
    <div style={{ width: "100%", padding: "10px" }}>
      <h3>AI Query</h3>

      <select
        value={selected}
        onChange={handleSelect}
        style={{ padding: "6px", marginBottom: "10px" }}
      >
        <option value="">-- choose model --</option>
        {files.map((f) => (
          <option key={f} value={f}>
            {f}
          </option>
        ))}
      </select>

      <br />

      <textarea
        style={{ width: "100%", height: "80px", marginBottom: "10px" }}        
        // placeholder="Enter GlobalId"
        placeholder='Enter a natural language query (e.g. count all windows), or paste raw request JSON (e.g. {"action": "get_parameters", "elementId": "751237", "parameters": ["Area"]}) to send it to /aiquery unchanged'

        value={query}
        onChange={(e) => setQuery(e.target.value)}
      />

      <br />

      <button onClick={runQuery}>
        Run Query
      </button>

      <pre style={{ marginTop: "10px", background: "#f5f5f5", padding: "10px" }}>
        {result}
      </pre>
    </div>
  );
}
