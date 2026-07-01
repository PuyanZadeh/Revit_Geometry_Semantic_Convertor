import React, { useState } from "react";
import api from "../api";              // FIX 1
import PluginCard from "../components/PluginCard";

export default function ConvertPanel() {
  const [file, setFile] = useState<File | null>(null);
  const [status, setStatus] = useState("");
  const [loading, setLoading] = useState(false);

  async function handleUpload() {
    if (!file) return;
    setLoading(true);
    setStatus("Uploading and converting...");

    try {
      console.log("FILE OBJECT:", file);

      const formData = new FormData();
      formData.append("file", file);

      const uploaded = await api.post("/convert", formData, {
        headers: { "Content-Type": "multipart/form-data" }
      });

      console.log("UPLOAD → server response", uploaded.data);
      if (uploaded.data.status === "ok") {
        setStatus("Upload and conversion completed!");
      } else {
        setStatus(uploaded.data.message || "Conversion failed");
      }

      /*
      setStatus("Upload and conversioin are completed!");
                  const jsonReq = { file: uploaded.data.ifc_path };
                  console.log("JSON → sending", jsonReq);
            
                  const converted = await api.post("/convert", jsonReq);
                  console.log("JSON → server response", converted.data);
            
                  setStatus(converted.data.status || "Conversion complete");
                */
    }
    catch {
      setStatus("Error during conversion");
    } finally {
      setLoading(false);
    }
  }

  return (
    <PluginCard name="IFC Convert Plugin">
      <div className="flex flex-col gap-3">
        <input
          type="file"
          accept=".ifc"
          onChange={(e) => setFile(e.target.files?.[0] || null)}
          className="border p-2 rounded"
        />
        <button
          onClick={handleUpload}
          disabled={!file || loading}
          className={`px-4 py-2 rounded-lg text-white ${loading ? "bg-gray-400" : "bg-blue-600 hover:bg-blue-700"
            }`}
        >
          {loading ? "Processing..." : "Upload & Convert"}
        </button>
        <pre className="text-sm bg-gray-100 p-2 rounded">
          {status || "No upload yet."}
        </pre>
      </div>
    </PluginCard>
  );
}
