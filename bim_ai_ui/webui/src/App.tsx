import React, { useState } from "react";
import * as Tabs from "@radix-ui/react-tabs";
import * as ScrollArea from "@radix-ui/react-scroll-area";
import { runConvert } from "./api/convert";
import { runGeom } from "./api/geom";
import { runAiquery } from "./api/aiquery";
import { runQc } from "./api/qc";
import { runSecurity } from "./api/security";
import LoggingPanel from "./plugins/LoggingPanel";
import SecurityPanel from "./plugins/SecurityPanel";
import ConvertPanel from "./plugins/ConvertPanel";
import ViewerPanel from "./plugins/ViewerPanel";
import SemanticFilterPanel from "./plugins/SemanticFilterPanel";

const App: React.FC = () => {
  const [output, setOutput] = useState("");
  const [selectedModel, setSelectedModel] = useState("");

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
          <Tabs.Trigger value="geom">Geom</Tabs.Trigger>
          <Tabs.Trigger value="aiquery">AI Query</Tabs.Trigger>
          <Tabs.Trigger value="qc">QC</Tabs.Trigger>
          <Tabs.Trigger value="security">Security</Tabs.Trigger>
        </Tabs.List>


        <Tabs.Content value="geom">
          <button onClick={() => runPlugin(runGeom)}>Run Geom</button>
        </Tabs.Content>
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
<LoggingPanel/>

<SecurityPanel/>
<h2 className="text-lg mt-6 font-semibold">Response:</h2>
<ConvertPanel/>

<ViewerPanel selectedModel={selectedModel} onSelectModel={setSelectedModel} />
<SemanticFilterPanel selectedModel={selectedModel} />
      
      <ScrollArea.Root className="h-60 w-full overflow-y-auto border p-3 rounded bg-gray-50">
        <pre>{output}</pre>
      </ScrollArea.Root>
    </div>
  ); 
};

export default App;
