// SemanticInspector.tsx
// Right-hand panel for the viewer page. Presents the AIQuery "get_element"
// response (selected_element, resolved_element, traversal_path, properties)
// as a progressively-disclosed BIM inspector instead of raw JSON dumps.
// The backend response shape is unchanged; this only reorganizes how it's shown.
import React from "react";
import SelectionSummary from "./SelectionSummary";
import TraversalChain from "./TraversalChain";
import PropertiesTable from "./PropertiesTable";
import AdvancedDebug from "./AdvancedDebug";
import { pickField } from "./fieldUtils";
import "./SemanticInspector.css";

export default function SemanticInspector({
  aiq,
  pickedId,
  aiqErr,
}: {
  aiq: any;
  pickedId: string;
  aiqErr: string;
}) {
  if (aiqErr) {
    return (
      <div className="si-panel">
        <div className="si-empty si-error">{aiqErr}</div>
      </div>
    );
  }

  if (!aiq) {
    return (
      <div className="si-panel">
        <div className="si-empty">Click an element in the viewer to inspect it.</div>
      </div>
    );
  }

  const selected = aiq.selected_element;
  const resolved = aiq.resolved_element;
  const traversal = aiq.traversal_path;
  const properties = aiq.properties;

  const contextCategory =
    pickField(resolved, ["Category", "category"]) ?? pickField(selected, ["Category", "category"]);
  const contextUniqueId =
    pickField(resolved, ["UniqueId", "uniqueId"]) ?? pickField(selected, ["UniqueId", "uniqueId"]);

  return (
    <div className="si-panel">
      <div className="si-meta">
        {pickedId ? <span>Revit ID: {pickedId}</span> : null}
        {aiq.GlobalId ? <span>GlobalId: {aiq.GlobalId}</span> : null}
        {Array.isArray(aiq.node_indices) ? <span>Nodes: {aiq.node_indices.length}</span> : null}
      </div>

      <section className="si-section">
        <div className="si-section-title">Selection</div>
        <SelectionSummary element={selected} fallback={resolved} />
      </section>

      <div className="si-divider" />

      <section className="si-section">
        <div className="si-section-title">Semantic Context</div>
        <div className="si-context-line">
          {contextCategory || contextUniqueId ? (
            <>
              <span className="si-context-category">{contextCategory || "Unknown"}</span>
              {contextUniqueId ? <span className="si-context-id">{contextUniqueId}</span> : null}
            </>
          ) : (
            <span className="si-empty">No semantic context resolved</span>
          )}
        </div>

        <div className="si-subtitle">Traversal</div>
        <TraversalChain path={traversal} firstElement={selected} lastElement={resolved} />
      </section>

      <div className="si-divider" />

      <section className="si-section">
        <div className="si-section-title">Properties</div>
        <PropertiesTable properties={properties} />
      </section>

      <div className="si-divider" />

      <AdvancedDebug
        blocks={[
          { label: "Clicked Object (raw)", data: selected },
          { label: "Semantic Context (raw)", data: resolved },
          { label: "Traversal (raw)", data: traversal },
        ]}
      />
    </div>
  );
}
