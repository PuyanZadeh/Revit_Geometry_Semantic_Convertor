// AdvancedDebug.tsx
// Collapsed by default. Holds the full raw JSON that used to be dumped
// directly on the page. Pass additional { label, data } blocks as needed
// for future debug information without changing the layout.
import React from "react";

export default function AdvancedDebug({
  blocks,
}: {
  blocks: Array<{ label: string; data: any }>;
}) {
  return (
    <details className="si-advanced">
      <summary>Advanced</summary>
      <div className="si-advanced-body">
        {blocks.map(({ label, data }) => (
          <div key={label} className="si-advanced-block">
            <div className="si-section-title">{label}</div>
            <pre className="si-json">{JSON.stringify(data, null, 2)}</pre>
          </div>
        ))}
      </div>
    </details>
  );
}
