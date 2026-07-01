import React, { useState } from "react";
import { runSecurity } from "../api/security";
import PluginCard from "../components/PluginCard";

export default function SecurityPanel() {
  const [user, setUser] = useState("puyan");
  const [role, setRole] = useState("admin");
  const [token, setToken] = useState("");
  const [error, setError] = useState("");

  async function handleGenerate() {
    setError("");
    try {
      const res = await runSecurity({ user, role });
      setToken(res.token || "");
    } catch (e: any) {
      setError("Error generating token");
    }
  }

  return (
    <PluginCard name="Security & Access Plugin">
      <div className="flex flex-col gap-3">
        <div className="flex gap-2">
  <input
    value={user}
    onChange={(e) => setUser(e.target.value)}
    placeholder="User"
    className="border rounded px-2 py-1 flex-1"
  />
  <select
    value={role}
    onChange={(e) => setRole(e.target.value)}
    className="border rounded px-2 py-1 flex-1 bg-white"
  >
    <option value="admin">admin</option>
    <option value="editor">editor</option>
    <option value="viewer">viewer</option>
  </select>
</div>


        <button
          onClick={handleGenerate}
          className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 w-fit"
        >
          Generate Token
        </button>

        {token && (
          <pre className="bg-gray-100 p-2 rounded text-sm break-all">
            {token}
          </pre>
        )}
        {error && <div className="text-red-600">{error}</div>}
      </div>
    </PluginCard>
  );
}
