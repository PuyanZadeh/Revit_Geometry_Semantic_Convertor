// PropertiesTable.tsx
import React, { useMemo, useState } from "react";
import { formatCellValue } from "./fieldUtils";

export default function PropertiesTable({
  properties,
}: {
  properties: Record<string, any> | null | undefined;
}) {
  const [filter, setFilter] = useState("");

  const entries = useMemo(
    () => (properties && typeof properties === "object" ? Object.entries(properties) : []),
    [properties]
  );

  const rows = useMemo(() => {
    if (!filter.trim()) return entries;
    const f = filter.toLowerCase();
    return entries.filter(
      ([k, v]) => k.toLowerCase().includes(f) || formatCellValue(v).toLowerCase().includes(f)
    );
  }, [entries, filter]);

  if (entries.length === 0) {
    return <div className="si-empty">No properties</div>;
  }

  return (
    <div>
      <input
        className="si-filter-input"
        placeholder={`Filter ${entries.length} properties…`}
        value={filter}
        onChange={(e) => setFilter(e.target.value)}
      />
      <table className="si-prop-table">
        <thead>
          <tr>
            <th>Property</th>
            <th>Value</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(([k, v]) => (
            <tr key={k}>
              <td className="si-prop-key">{k}</td>
              <td className="si-prop-value">{formatCellValue(v)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {filter && rows.length === 0 ? <div className="si-empty">No matches</div> : null}
    </div>
  );
}
