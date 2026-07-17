import React, { useState } from "react";
import * as Tabs from "@radix-ui/react-tabs";
import * as ScrollArea from "@radix-ui/react-scroll-area";
import { runConvert } from "./api/convert";
import { runAiquery } from "./api/aiquery";
import { runQc } from "./api/qc";
import { runSecurity } from "./api/security";
import LoggingPanel from "./plugins/LoggingPanel";
import SecurityPanel from "./plugins/SecurityPanel";
import ConvertPanel from "./plugins/ConvertPanel";
import ViewerPanel from "./plugins/ViewerPanel";
import SemanticFilterPanel from "./plugins/SemanticFilterPanel";
import GeomPanel from "./plugins/GeomPanel";

const App: React.FC = () => {
  const [output, setOutput] = useState("");
  const [selectedModel, setSelectedModel] = useState("");
  const [selectedElementId, setSelectedElementId] = useState("");

  const [showGeom, setShowGeom] = useState(false);
  const [showSemantic, setShowSemantic] = useState(false);
  const [showLogging, setShowLogging] = useState(false);
  const [showSecurity, setShowSecurity] = useState(false);
  const [showConvert, setShowConvert] = useState(false);

  // Picking a new model invalidates whatever element was previously picked
  // (it belonged to the old model's geometry), so clear it here rather than
  // leaving a stale reference visible to other tabs.
  function handleSelectModel(file: string) {
    setSelectedModel(file);
    setSelectedElementId("");
  }

  const runPlugin = async (fn: () => Promise<any>) => {
    try {
      const res = await fn();
      setOutput(JSON.stringify(res, null, 2));
    } catch {
      setOutput("Error");
    }
  };

  return (
    <div className="p-6 font-sans" style={{ padding: "24px", boxSizing: "border-box" }}>
      <h1 className="text-2xl font-bold mb-4">AI-BIM Dashboard</h1>
      <Tabs.Root defaultValue="convert">
        <Tabs.List className="flex gap-2 border-b pb-2">
          <Tabs.Trigger value="convert">Convert</Tabs.Trigger>
          <Tabs.Trigger value="aiquery">AI Query</Tabs.Trigger>
          <Tabs.Trigger value="qc">QC</Tabs.Trigger>
          <Tabs.Trigger value="security">Security</Tabs.Trigger>
        </Tabs.List>

        <Tabs.Content value="aiquery">
          <button onClick={() => runPlugin(runAiquery)}>Run AI Query</button>
        </Tabs.Content>
        <Tabs.Content value="qc">
          <button onClick={() => runPlugin(runQc)}>Run QC</button>
        </Tabs.Content>
        <Tabs.Content value="security">
          <button onClick={() => runPlugin(runSecurity)}>Run Security</button>
        </Tabs.Content>
      </Tabs.Root>

      {/* Row 1: utility plugins, collapsed behind toggle buttons, default hidden. */}
      <div className="flex gap-2 mt-4">
        <button onClick={() => setShowLogging((v) => !v)}>Logging Plugin</button>
        <button onClick={() => setShowSecurity((v) => !v)}>Security &amp; Access Plugin</button>
        <button onClick={() => setShowConvert((v) => !v)}>IFC Convert Plugin</button>
      </div>

      {/* Row 2: query interfaces (geometry engine, semantic engine), collapsed behind toggle buttons, default hidden. */}
      <div className="flex gap-2 mt-2">
        <button onClick={() => setShowGeom((v) => !v)}>Geom Query</button>
        <button onClick={() => setShowSemantic((v) => !v)}>Semantic Query</button>
      </div>

      {showLogging && <LoggingPanel />}
      {showSecurity && <SecurityPanel />}
      <h2 className="text-lg mt-6 font-semibold">Response:</h2>
      {showConvert && <ConvertPanel />}

      <ViewerPanel
        selectedModel={selectedModel}
        onSelectModel={handleSelectModel}
        selectedElementId={selectedElementId}
        onSelectElement={setSelectedElementId}
      />
      {showGeom && <GeomPanel selectedModel={selectedModel} selectedElementId={selectedElementId} />}
      {showSemantic && <SemanticFilterPanel selectedModel={selectedModel} />}

      <ScrollArea.Root className="h-60 w-full overflow-y-auto border p-3 rounded bg-gray-50">
        <pre>{output}</pre>
      </ScrollArea.Root>
    </div>
  ); 
};

export default App;
