// info.manager.ts
export function pickRevitId_info(
  nodeIndex: number,
  gltfJsonRef: React.RefObject<any>,
  ifcMapRef: React.RefObject<Record<string, number[]> | null>,
  revitMapRef: React.RefObject<any>
) {
  // IMPORTANT: get revitID first (needed for debugging + potential direct lookup)
  const revitID = gltfJsonRef.current.nodes?.[nodeIndex]?.revitID;
  console.log("Picked nodeIndex:", nodeIndex, "revitID:", revitID);

  // Map lookup: GlobalId -> [nodeIndex]  (or sometimes RevitUniqueId -> [nodeIndex])
  let globalId = "";
  const m = ifcMapRef.current;
/*
  if (m) {
    // fast path: if map is keyed by revitID (Revit addin style), check directly
    if (revitID && typeof m[revitID] !== "undefined") {
      const arr = m[revitID];
      if (Array.isArray(arr)) {
        const hit = arr.some((n: any) => Number(n) === Number(nodeIndex));
        if (hit) globalId = revitID;
      }
    }

    // fallback: scan all keys (IFC style maps are keyed by IFC GlobalId)
    if (!globalId) {
      for (const [gid, nodes] of Object.entries(m)) {
        if (!Array.isArray(nodes)) continue;
        const hit = nodes.some((n: any) => Number(n) === Number(nodeIndex));
        if (hit) {
          globalId = gid;
          break;
        }
      }
    }
  }
*/
if (m) {
  for (const [gid, nodes] of Object.entries(m)) {
    if (!Array.isArray(nodes)) continue;
    if (nodes.includes(nodeIndex)) {
      globalId = gid;
      break;
    }
  }
}


  //const intId = revitMapRef.current?.[revitID]?.intId;
  let intId: number | undefined = undefined;
  if (revitID && revitMapRef.current) {
    const hit = Object.values(revitMapRef.current).find(
      (v: any) => v?.uniqueId === revitID
    ) as any;
    intId = hit?.intId;
  }

  return { revitID, globalId, intId };
}

/*
    // add this function near your other AIQuery calls
    async function searchElements(modelMapPath: string, q: string) {
      const body = {
        plugin: "AIQuery",
        action: "search_elements",
        model: modelMapPath,   // full path to .map.json, same as validate_id
        query: q
      };
  
      const res = await fetch("/api/plugin", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body)
      });
  
      return await res.json();
    }
    
    // AI Query Call
    try {
      setAiqErr("");
      const r = await api.post("/aiquery", {
        action: "validate_id",
        model: selected.replace(/\.glb$/i, ".map.json"),
        globalId,
    });
    setAiq(r.data);
    } catch (e: any) {
        setAiq(null);
        setAiqErr("aiquery failed");
    }
    */

