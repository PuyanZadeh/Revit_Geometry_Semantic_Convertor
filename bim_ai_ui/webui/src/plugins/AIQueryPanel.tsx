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
  if (!selected || !query) return;

  const req = {
    action: "nl_query",
    model: selected,
    nl: query
  };

  const res = await api.post("/aiquery", req);
  setResult(JSON.stringify(res.data, null, 2));
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
        placeholder="Enter natural language query (e.g. count all windows)"

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
