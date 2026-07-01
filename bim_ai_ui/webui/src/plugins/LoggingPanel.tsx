import React, { useState, useEffect } from "react";
import { runLogging } from "../api/logging";
import PluginCard from "../components/PluginCard";

export default function LoggingPanel() {
  const [logs, setLogs] = useState<string[]>([]);
  const [filter, setFilter] = useState("");

  async function fetchLogs() {
    try {
      const res = await runLogging({ action: "get" });
      if (Array.isArray(res.logs)) setLogs(res.logs);
    } catch (e) {
      console.error(e);
    }
  }

  async function clearLogs() {
    try {
      await runLogging({ action: "clear" });
      setLogs([]);
    } catch (e) {
      console.error(e);
    }
  }

  useEffect(() => {
    fetchLogs();
  }, []);

  const filtered = logs.filter(l => l.toLowerCase().includes(filter.toLowerCase()));

  return (
    <PluginCard name="Logging Plugin">
      <div className="flex items-center gap-2 mb-3">
        <button
          onClick={fetchLogs}
          className="px-3 py-1 bg-blue-600 text-white rounded-lg hover:bg-blue-700"
        >
          Refresh
        </button>
        <button
          onClick={clearLogs}
          className="px-3 py-1 bg-red-600 text-white rounded-lg hover:bg-red-700"
        >
          Clear
        </button>
        <input
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
          placeholder="Filter logs..."
          className="border rounded-lg px-2 py-1 flex-1"
        />
      </div>
      <div className="bg-gray-100 p-2 rounded max-h-96 overflow-y-auto text-sm whitespace-pre-wrap">
        {filtered.length > 0 ? filtered.join("\n") : "No logs available."}
      </div>
    </PluginCard>
  );
}
