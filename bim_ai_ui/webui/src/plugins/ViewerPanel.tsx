// ViewerPanel.tsx
import React, { useEffect, useState, useRef } from "react";
import api from "../api";
import * as THREE from "three";
// import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls";

import { loadModel } from "../viewer/model.manager";
import { pickRevitId_interaction } from "../viewer/interaction.manager";
import { pickRevitId_info } from "../viewer/info.manager";
import SemanticInspector from "../components/semantic-inspector/SemanticInspector";
import "./ViewerPanel.css";

export default function ViewerPanel() {
  const [files, setFiles] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>("");

  const containerRef = useRef<HTMLDivElement>(null);
  const rendererRef = useRef<THREE.WebGLRenderer | null>(null);
  const sceneRef = useRef<THREE.Scene | null>(null);
  const cameraRef = useRef<THREE.PerspectiveCamera | null>(null);

  const controlsRef = useRef<OrbitControls | null>(null);

  const modelRef = useRef<THREE.Object3D | null>(null);
  const fitRef = useRef<{ center: THREE.Vector3; distance: number } | null>(null);

  const [pickedId, setPickedId] = useState<string>("");
  const [aiq, setAiq] = useState<any>(null);

  const assocRef = useRef<Map<any, any> | null>(null);
  const gltfJsonRef = useRef<any>(null);
  const revitMapRef = useRef<any>(null);
  const revitMetaRef = useRef<Record<string, any> | null>(null);

  const ifcMapRef = useRef<Record<string, number[]> | null>(null);

  const [aiqErr, setAiqErr] = useState<string>("");

  const ifcIndexToGidRef = useRef<Map<number, string> | null>(null);

  useEffect(() => {
    async function loadList() {
      const req = { action: "list_glb" };
      const res = await api.post("/visualization", req);
      const sorted = (res.data.files || []).sort();
      setFiles(sorted);
    }
    loadList();
  }, []);
  useEffect(() => {
    async function loadRevitMeta() {
      const jsonFile = selected.replace(/\.glb$/i, ".json");
      // console.log("requesting revit id map:", jsonFile);

      const res = await api.post("/visualization", {
        action: "get_revit_id_map",
        file: encodeURIComponent(jsonFile),
      });

      //     console.log("revit id map response keys:", Object.keys(res.data || {}).length);
      // console.log("revit id map response top-level keys:", Object.keys(res.data || {}));
      // console.log("revit id map response sample:", res.data);

      revitMapRef.current = res.data;
    }

    if (selected) loadRevitMeta();
  }, [selected]);

  useEffect(() => {
    async function loadIfcMap() {
      const mapFile = selected.replace(/\.glb$/i, ".map.json");
      const url = `http://192.168.0.150/storage/outputs/gltf/${encodeURIComponent(mapFile)}`;
      /*
          const res = await api.post("/visualization", {
            action: "get_file_json",
            file: encodeURIComponent(mapFile),
            //file: mapFile,
          });
      
      console.log("IFC MAP raw response:", res.data);
      
      if (res.data && typeof res.data === "object" && (res.data as any).error) {
        console.log("IFC MAP backend error:", (res.data as any).error, (res.data as any).path || "");
        ifcMapRef.current = null;
        return;
      }
      
      
          let data: any = res.data;
      
          // backend might return file contents as a string
          if (typeof data === "string") {
            try {
              data = JSON.parse(data);
            } catch {
              data = null;
            }
          }
      
          // backend might wrap the payload
          if (data && typeof data === "object" && !Array.isArray(data)) {
            if (data.map && typeof data.map === "object") data = data.map;
            if (data.mapping && typeof data.mapping === "object") data = data.mapping;
          }
      */

      const res = await fetch(url, { cache: "no-store" });
      if (!res.ok) {
        ifcMapRef.current = null;
        console.log("IFC MAP fetch failed:", res.status, url);
        return;
      }

      const data = await res.json();
      ifcMapRef.current = (data && typeof data === "object") ? data : null;

      const rev = new Map<number, string>();
      for (const [gid, arr] of Object.entries(data)) {
        if (!Array.isArray(arr)) continue;
        for (const idx of arr) {
          if (typeof idx === "number") rev.set(idx, gid);
        }
      }
      ifcIndexToGidRef.current = rev;

      console.log("IFC reverse map size:", rev.size);

      console.log("IFC MAP loaded keys:", Object.keys(ifcMapRef.current || {}).length);
      console.log("IFC MAP first key:", Object.keys(ifcMapRef.current || {})[0] || "none");

      console.log("IFC MAP loaded:", typeof ifcMapRef.current, Array.isArray(ifcMapRef.current));
      console.log("IFC MAP keys:", Object.keys(ifcMapRef.current || {}).length);
      //console.log("IFC MAP keys:", ifcMapRef.current ? Object.keys(ifcMapRef.current).length : 0);
      //console.log("IFC MAP first:", ifcMapRef.current ? Object.keys(ifcMapRef.current)[0] : "none");
    }

    if (selected) loadIfcMap();
  }, [selected]);

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
  */

  function initViewer() {
    const container = containerRef.current;
    if (!container) return;

    const width = container.clientWidth || 800;
    const height = container.clientHeight || 600;

    const scene = new THREE.Scene();
    //    scene.background = new THREE.Color(0xffffff);
    scene.background = new THREE.Color(0x1e1e1e);

    sceneRef.current = scene;

    const camera = new THREE.PerspectiveCamera(60, width / height, 0.1, 5000);
    camera.position.set(5, 5, 10);
    cameraRef.current = camera;

    // clear any previous canvas
    container.innerHTML = "";

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(window.devicePixelRatio);
    renderer.setSize(width, height);
    rendererRef.current = renderer;
    container.appendChild(renderer.domElement);

    controlsRef.current = new OrbitControls(camera, renderer.domElement);
    controlsRef.current.enableDamping = true;
    controlsRef.current!.enablePan = true;

    //  scene.add(new THREE.DirectionalLight(0xffffff, 1).position.set(10, 10, 10));
    //const d = new THREE.DirectionalLight(0xffffff, 1);
    const d = new THREE.DirectionalLight(0xffffff, 0.8);
    d.position.set(10, 10, 10);
    scene.add(d);

    //scene.add(new THREE.AmbientLight(0xffffff, 0.5));
    scene.add(new THREE.AmbientLight(0xffffff, 0.3));

    const onResize = () => {
      const c = containerRef.current;
      if (!c || !rendererRef.current || !cameraRef.current) return;

      const w = c.clientWidth || 800;
      const h = c.clientHeight || 600;

      rendererRef.current.setSize(w, h);
      cameraRef.current.aspect = w / h;
      cameraRef.current.updateProjectionMatrix();
    };
    window.addEventListener("resize", onResize);

    const animate = () => {
      requestAnimationFrame(animate);
      controlsRef.current?.update();
      renderer.render(scene, camera);
    };
    animate();
  }

  useEffect(() => {
    initViewer();
  }, []);

  function zoomAll() {
    const fit = fitRef.current;
    const camera = cameraRef.current;
    if (!fit || !camera) return;

    camera.near = fit.distance / 100;
    camera.far = fit.distance * 100;
    camera.setViewOffset(0, 0, 0, 0, 0, 0); // no-op, but keeps TS happy in some configs
    camera.updateProjectionMatrix();

    camera.position.set(fit.center.x, fit.center.y, fit.center.z + fit.distance);
    camera.lookAt(fit.center);

    controlsRef.current?.target.copy(fit.center);
    controlsRef.current?.update();
  }

  function setView(dir: "top" | "east" | "north") {
    const fit = fitRef.current;
    const camera = cameraRef.current;
    if (!fit || !camera) return;

    const d = fit.distance;
    const c = fit.center;

    if (dir === "top") camera.position.set(c.x, c.y + d, c.z);
    if (dir === "east") camera.position.set(c.x + d, c.y, c.z);
    if (dir === "north") camera.position.set(c.x, c.y, c.z + d);

    camera.lookAt(c);
    controlsRef.current?.target.copy(c);
    controlsRef.current?.update();
  }

  async function pickRevitId(e: React.MouseEvent<HTMLDivElement>) {
    if (
      !cameraRef.current ||
      !modelRef.current ||
      !containerRef.current ||
      !assocRef.current ||
      !gltfJsonRef.current
    ) {
      return;
    }

    // const rect = containerRef.current.getBoundingClientRect();
    // const mouse = new THREE.Vector2(
    //   ((e.clientX - rect.left) / rect.width) * 2 - 1,
    //   -(((e.clientY - rect.top) / rect.height) * 2 - 1)
    // );
    //
    // const raycaster = new THREE.Raycaster();
    // raycaster.setFromCamera(mouse, cameraRef.current);
    //
    // const hits = raycaster.intersectObject(modelRef.current, true);
    // if (!hits.length) return;
    //
    // // walk up from mesh -> parent until we find a glTF NODE association
    // let o: any = hits[0].object;
    //
    // const p = hits[0].point;
    // controlsRef.current?.target.copy(p);
    // controlsRef.current?.update();
    //
    // let nodeIndex: number | null = null;
    // // let meshIndex: number | null = null;
    //
    // while (o) {
    //   const a = assocRef.current.get(o);
    //
    //   console.log("a =", a);
    //   console.log("keys:", a && Object.keys(a));
    //   console.log("o.name:", o?.name, "o.userData:", o?.userData);
    //
    //   if (typeof a?.meshes === "number") {
    //     nodeIndex = a.meshes;
    //     console.log("a.meshes =", a.meshes);
    //     break;
    //   }
    //   /*
    //         //      if (a?.type === "nodes") {
    //         if (a?.nodes !== undefined) {
    //           //if (a?.type === "nodes" && typeof a.index === "number") {
    //   
    //           //nodeIndex = a.index;
    //           //console.log("a.index =", a.index);
    //           nodeIndex = Number(a.nodes);
    //           console.log("a.nodes =", a.nodes);
    //           break;
    //         }
    //   
    //   
    //         if (a?.type === "meshes") {
    //           meshIndex = a.index;
    //           const nodes = (gltfJsonRef.current.nodes as any[]) || [];
    //           nodeIndex = nodes.findIndex((n) => n.mesh === meshIndex);
    //           break;
    //         }
    //         */
    //
    //   o = o.parent;
    // }
    //
    // if (nodeIndex == null) {
    //   setPickedId("");
    //   console.log("Picked: no node association found");
    //   return;
    // }

    const interaction = pickRevitId_interaction(
      e,
      containerRef,
      cameraRef,
      modelRef,
      assocRef,
      controlsRef
    );

    let nodeIndex: number | null = interaction.nodeIndex;
    if (nodeIndex == null) {
      setPickedId("");
      console.log("Picked: no node association found");
      return;
    }

    // IMPORTANT: get revitID first (needed for debugging + potential direct lookup)
    // const revitID = gltfJsonRef.current.nodes?.[nodeIndex]?.revitID;
    // console.log("Picked nodeIndex:", nodeIndex, "revitID:", revitID);
    //
    // // Map lookup: GlobalId -> [nodeIndex]  (or sometimes RevitUniqueId -> [nodeIndex])
    // let globalId = "";
    // const m = ifcMapRef.current;
    //
    // if (m) {
    //   // fast path: if map is keyed by revitID (Revit addin style), check directly
    //   if (revitID && typeof m[revitID] !== "undefined") {
    //     const arr = m[revitID];
    //     if (Array.isArray(arr)) {
    //       const hit = arr.some((n: any) => Number(n) === Number(nodeIndex));
    //       if (hit) globalId = revitID;
    //     }
    //   }
    //
    //   // fallback: scan all keys (IFC style maps are keyed by IFC GlobalId)
    //   if (!globalId) {
    //     for (const [gid, nodes] of Object.entries(m)) {
    //       if (!Array.isArray(nodes)) continue;
    //       const hit = nodes.some((n: any) => Number(n) === Number(nodeIndex));
    //       if (hit) {
    //         globalId = gid;
    //         break;
    //       }
    //     }
    //   }
    // }
    //
    // if (!globalId) {
    //   const firstKey = m ? Object.keys(m)[0] : "";
    //   console.log("Picked: no GlobalId found for nodeIndex", nodeIndex);
    //   console.log("Map first key sample:", firstKey);
    //   return;
    // }

    const info = pickRevitId_info(
      nodeIndex,
      gltfJsonRef,
      ifcMapRef,
      revitMapRef
    );

    const revitID = info.revitID;
    let globalId = info.globalId;

    if (!globalId) {
      const firstKey = ifcMapRef.current ? Object.keys(ifcMapRef.current)[0] : "";
      console.log("Picked: no GlobalId found for nodeIndex", nodeIndex);
      console.log("Map first key sample:", firstKey);
      return;
    }

    // AI Query Call
    try {
      setAiqErr("");
      const r = await api.post("/aiquery", {
        action: "get_element",
        //action: "validate_id",
        model: selected.replace(/\.glb$/i, ".map.json"),
        // model: `http://192.168.0.150/storage/outputs/gltf/${encodeURIComponent(selected.replace(/\.glb$/i, ".map.json"))}`,
        globalId,
      });

console.log("nodeIndex: ", nodeIndex);
console.log("globalId: ", globalId);
console.log("model path:", `http://192.168.0.150/storage/outputs/gltf/${encodeURIComponent(selected.replace(/\.glb$/i, ".map.json"))}`,);

      setAiq(r.data);
    } catch (e: any) {
      setAiq(null);
      setAiqErr("aiquery failed");
    }

    //const intId = revitMapRef.current?.[revitID]?.intId;
    // let intId: number | undefined = undefined;
    // if (revitID && revitMapRef.current) {
    //   const hit = Object.values(revitMapRef.current).find(
    //     (v: any) => v?.uniqueId === revitID
    //   ) as any;
    //   intId = hit?.intId;
    // }
    let intId: number | undefined = info.intId;

    setPickedId(String(revitID ?? ""));
    //setPickedId(String(intId ?? ""));

    console.log("Picked node:", nodeIndex, "revitID:", revitID, "revitIntID:", intId, "globalId:", globalId);
  }

  // async function loadModel(file: string) {
  //   if (!sceneRef.current || !cameraRef.current) return;
  //
  //   // const path = `https://bim-ai.bimbc.com/storage/outputs/gltf/${file}`;
  //   // const path = `https://bim-ai.bimbc.com/storage/outputs/gltf/${encodeURIComponent(file)}`;
  //   /* adjustment for the local running. the second line above runs well on a Google Instance*/
  //   //const path = `http://127.0.0.1/storage/outputs/gltf/${encodeURIComponent(file)}`; //removed the S in https!
  //   const path = `http://192.168.0.150/storage/outputs/gltf/${encodeURIComponent(file)}`; //removed the S in https!
  //
  //   const loader = new GLTFLoader();
  //   console.log("Loading:", path);
  //
  //   loader.load(
  //     path,
  //     (gltf: any) => {
  //       const scene = sceneRef.current!;
  //       const camera = cameraRef.current!;
  //
  //       // clear scene
  //       while (scene.children.length > 0) {
  //         scene.remove(scene.children[0]);
  //       }
  //
  //       const model = gltf.scene;
  //       scene.add(model);
  //
  //       modelRef.current = model;
  //
  //       // store glTF node associations + json for picking node index later
  //       assocRef.current = (gltf.parser as any).associations || null;
  //       gltfJsonRef.current = gltf.parser?.json || null;
  //
  //       console.log("associations present:", !!assocRef.current);
  //       console.log("associations size:", (assocRef.current as any)?.size);
  //       console.log("json nodes count:", gltfJsonRef.current?.nodes?.length);
  //       console.log("revit meta loaded keys:", Object.keys(revitMapRef.current || {}).length);
  //
  //       console.log(model.children.length);
  //       //        (gltf.parser.json.nodes as any[]).forEach((node, index) => console.log(index, node.name));
  //
  //       // lighting
  //       scene.add(new THREE.AmbientLight(0xffffff, 0.9));
  //
  //       const dir = new THREE.DirectionalLight(0xffffff, 1.2);
  //       dir.position.set(10, 20, 10);
  //       scene.add(dir);
  //
  //       // fit camera
  //       const box = new THREE.Box3().setFromObject(model);
  //       const size = box.getSize(new THREE.Vector3());
  //       const center = box.getCenter(new THREE.Vector3());
  //
  //       const maxDim = Math.max(size.x, size.y, size.z);
  //       const fov = camera.fov * (Math.PI / 180);
  //       const distance = (maxDim / (2 * Math.tan(fov / 2))) * 1.2;
  //
  //       fitRef.current = { center: center.clone(), distance };
  //
  //       camera.position.set(center.x, center.y, center.z + distance);
  //
  //       camera.near = distance / 100;
  //       camera.far = distance * 100;
  //       camera.updateProjectionMatrix();
  //       camera.lookAt(center);
  //
  //       controlsRef.current?.target.copy(center);
  //       controlsRef.current?.update();
  //
  //       console.log("Loaded. Scene children:", gltf.scene?.children?.length);
  //
  //       /**
  //        *
  //       const box = new THREE.Box3().setFromObject(model);
  //       const size = box.getSize(new THREE.Vector3());
  //       const center = box.getCenter(new THREE.Vector3());
  //       
  //       const maxDim = Math.max(size.x, size.y, size.z);
  //       const fov = camera.fov * (Math.PI / 180);
  //       let cameraZ = Math.abs(maxDim / (2 * Math.tan(fov / 2)));
  //       cameraZ *= 1.5;
  //       
  //       camera.position.set(center.x, center.y + cameraZ * 0.2, center.z + cameraZ);
  //       camera.lookAt(center);
  //       */
  //     },
  //     undefined,
  //     (error: any) => console.error("GLB load error:", error)
  //   );
  // }

  async function loadModel_wrapper(file: string) {
    await loadModel(
      file,
      sceneRef,
      cameraRef,
      controlsRef,
      modelRef,
      assocRef,
      gltfJsonRef,
      revitMapRef,
      fitRef
    );
  }

  function handleSelect(e: React.ChangeEvent<HTMLSelectElement>) {
    const file = e.target.value;
    setSelected(file);
    if (file) loadModel_wrapper(file);
  }

  return (
    <div className="vp-page">
      <div className="vp-toolbar">
        <div className="vp-toolbar-row">
          <select value={selected} onChange={handleSelect} style={{ padding: "6px" }}>
            <option value="">-- choose file --</option>
            {files.map((f) => (
              <option key={f} value={f}>
                {f}
              </option>
            ))}
          </select>

          <div className="vp-view-buttons">
            <button onClick={zoomAll}>Zoom All</button>
            <button onClick={() => setView("top")}>Top</button>
            <button onClick={() => setView("east")}>East</button>
            <button onClick={() => setView("north")}>North</button>
          </div>
        </div>
      </div>

      <div className="vp-body">
        <div className="vp-viewer-col">
          <div ref={containerRef} onClick={pickRevitId} className="vp-canvas" />
        </div>
        <div className="vp-inspector-col">
          <SemanticInspector aiq={aiq} pickedId={pickedId} aiqErr={aiqErr} />
        </div>
      </div>
    </div>
  );
}