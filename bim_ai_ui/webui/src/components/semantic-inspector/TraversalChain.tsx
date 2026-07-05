// TraversalChain.tsx
// Renders the backend's traversal_path (an array of uniqueId strings) as a
// visual breadcrumb instead of a raw JSON array. The first and last hops are
// labeled with their known category (from selected/resolved element) when
// available; intermediate hops fall back to their raw uniqueId.
import React from "react";
import { pickField } from "./fieldUtils";

export default function TraversalChain({
  path,
  firstElement,
  lastElement,
}: {
  path: any;
  firstElement?: any;
  lastElement?: any;
}) {
  const ids: string[] = Array.isArray(path) ? path.filter(Boolean) : [];

  if (ids.length === 0) {
    return <div className="si-empty">No traversal recorded</div>;
  }

  const labelFor = (id: string, index: number) => {
    const el = index === 0 ? firstElement : index === ids.length - 1 ? lastElement : null;
    const category = el ? pickField(el, ["Category", "category"]) : null;
    return category ? String(category) : id;
  };

  return (
    <div className="si-chain">
      {ids.map((id, i) => (
        <React.Fragment key={`${id}-${i}`}>
          {i > 0 ? <span className="si-chain-arrow">&rarr;</span> : null}
          <span className="si-chip" title={id}>
            {labelFor(id, i)}
          </span>
        </React.Fragment>
      ))}
    </div>
  );
}
