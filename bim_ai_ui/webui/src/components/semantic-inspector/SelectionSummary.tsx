// SelectionSummary.tsx
import React from "react";
import { pickField } from "./fieldUtils";

const FIELDS: Array<{ label: string; keys: string[] }> = [
  { label: "Category", keys: ["Category", "category"] },
  { label: "UniqueId", keys: ["UniqueId", "uniqueId", "uniqueID"] },
  { label: "Type", keys: ["Type", "type", "typeName", "Type Name"] },
  { label: "HostId", keys: ["HostId", "hostId"] },
  { label: "ParentId", keys: ["ParentId", "parentId"] },
  { label: "LevelId", keys: ["LevelId", "levelId"] },
];

export default function SelectionSummary({
  element,
  fallback,
}: {
  element: any;
  fallback?: any;
}) {
  return (
    <div className="si-kv-grid">
      {FIELDS.map(({ label, keys }) => {
        const value = pickField(element, keys) ?? pickField(fallback, keys);
        return (
          <React.Fragment key={label}>
            <div className="si-kv-label">{label}</div>
            <div className="si-kv-value">
              {value !== undefined ? String(value) : "—"}
            </div>
          </React.Fragment>
        );
      })}
    </div>
  );
}
