// model.manager.ts
import * as THREE from "three";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls";

export async function loadModel(
  file: string,
  sceneRef: React.RefObject<THREE.Scene | null>,
  cameraRef: React.RefObject<THREE.PerspectiveCamera | null>,
  controlsRef: React.RefObject<OrbitControls | null>,
  modelRef: React.RefObject<THREE.Object3D | null>,
  assocRef: React.RefObject<Map<any, any> | null>,
  gltfJsonRef: React.RefObject<any>,
  revitMapRef: React.RefObject<any>,
  fitRef: React.RefObject<{ center: THREE.Vector3; distance: number } | null>
) {
  if (!sceneRef.current || !cameraRef.current) return;

  // const path = `https://bim-ai.bimbc.com/storage/outputs/gltf/${file}`;
  // const path = `https://bim-ai.bimbc.com/storage/outputs/gltf/${encodeURIComponent(file)}`;
  /* adjustment for the local running. the second line above runs well on a Google Instance*/
  //const path = `http://127.0.0.1/storage/outputs/gltf/${encodeURIComponent(file)}`; //removed the S in https!
  const path = `http://192.168.0.150/storage/outputs/gltf/${encodeURIComponent(file)}`; //removed the S in https!

  const loader = new GLTFLoader();
  console.log("Loading:", path);

  loader.load(
    path,
    (gltf: any) => {
      const scene = sceneRef.current!;
      const camera = cameraRef.current!;

      // clear scene
      while (scene.children.length > 0) {
        scene.remove(scene.children[0]);
      }

      const model = gltf.scene;
      scene.add(model);

      modelRef.current = model;

      // store glTF node associations + json for picking node index later
      assocRef.current = (gltf.parser as any).associations || null;
      gltfJsonRef.current = gltf.parser?.json || null;

      console.log("associations present:", !!assocRef.current);
      console.log("associations size:", (assocRef.current as any)?.size);
      console.log("json nodes count:", gltfJsonRef.current?.nodes?.length);
      console.log("revit meta loaded keys:", Object.keys(revitMapRef.current || {}).length);

      console.log(model.children.length);
      //        (gltf.parser.json.nodes as any[]).forEach((node, index) => console.log(index, node.name));

      // lighting
      scene.add(new THREE.AmbientLight(0xffffff, 0.9));

      const dir = new THREE.DirectionalLight(0xffffff, 1.2);
      dir.position.set(10, 20, 10);
      scene.add(dir);

      // fit camera
      const box = new THREE.Box3().setFromObject(model);
      const size = box.getSize(new THREE.Vector3());
      const center = box.getCenter(new THREE.Vector3());

      const maxDim = Math.max(size.x, size.y, size.z);
      const fov = camera.fov * (Math.PI / 180);
      const distance = (maxDim / (2 * Math.tan(fov / 2))) * 1.2;

      fitRef.current = { center: center.clone(), distance };

      camera.position.set(center.x, center.y, center.z + distance);

      camera.near = distance / 100;
      camera.far = distance * 100;
      camera.updateProjectionMatrix();
      camera.lookAt(center);

      controlsRef.current?.target.copy(center);
      controlsRef.current?.update();

      console.log("Loaded. Scene children:", gltf.scene?.children?.length);

      /**
       *
      const box = new THREE.Box3().setFromObject(model);
      const size = box.getSize(new THREE.Vector3());
      const center = box.getCenter(new THREE.Vector3());
      
      const maxDim = Math.max(size.x, size.y, size.z);
      const fov = camera.fov * (Math.PI / 180);
      let cameraZ = Math.abs(maxDim / (2 * Math.tan(fov / 2)));
      cameraZ *= 1.5;
      
      camera.position.set(center.x, center.y + cameraZ * 0.2, center.z + cameraZ);
      camera.lookAt(center);
      */
    },
    undefined,
    (error: any) => console.error("GLB load error:", error)
  );
}

/*import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls";


const camera: THREE.PerspectiveCamera;

const modelRef = useRef<THREE.Object3D | null>(null);
  const containerRef = useRef<HTMLDivElement>(null);
const assocRef = useRef<Map<any, any> | null>(null);
  const gltfJsonRef = useRef<any>(null);
  const controlsRef = useRef<OrbitControls | null>(null);
  const ifcMapRef = useRef<Record<string, number[]> | null>(null);
  const revitMapRef = useRef<any>(null);



async function pickRevitId(e: React.MouseEvent<HTMLDivElement>) {
    if (
      !camera.current ||
      !modelRef.current ||
      !containerRef.current ||
      !assocRef.current ||
      !gltfJsonRef.current
    ) {
      return;
    }

    const rect = containerRef.current.getBoundingClientRect();
    const mouse = new THREE.Vector2(
      ((e.clientX - rect.left) / rect.width) * 2 - 1,
      -(((e.clientY - rect.top) / rect.height) * 2 - 1)
    );

    const raycaster = new THREE.Raycaster();
    raycaster.setFromCamera(mouse, cameraRef.current);

    const hits = raycaster.intersectObject(modelRef.current, true);
    if (!hits.length) return;

    // walk up from mesh -> parent until we find a glTF NODE association
    let o: any = hits[0].object;

    const p = hits[0].point;
    controlsRef.current?.target.copy(p);
    controlsRef.current?.update();

    let nodeIndex: number | null = null;
    // let meshIndex: number | null = null;

    while (o) {
      const a = assocRef.current.get(o);

      console.log("a =", a);
      console.log("keys:", a && Object.keys(a));
      console.log("o.name:", o?.name, "o.userData:", o?.userData);

      if (typeof a?.meshes === "number") {
        nodeIndex = a.meshes;
        console.log("a.meshes =", a.meshes);
        break;
      }
      /*
            //      if (a?.type === "nodes") {
            if (a?.nodes !== undefined) {
              //if (a?.type === "nodes" && typeof a.index === "number") {
      
              //nodeIndex = a.index;
              //console.log("a.index =", a.index);
              nodeIndex = Number(a.nodes);
              console.log("a.nodes =", a.nodes);
              break;
            }
      
      
            if (a?.type === "meshes") {
              meshIndex = a.index;
              const nodes = (gltfJsonRef.current.nodes as any[]) || [];
              nodeIndex = nodes.findIndex((n) => n.mesh === meshIndex);
              break;
            }
            */
/*
      o = o.parent;
    }

    if (nodeIndex == null) {
      setPickedId("");
      console.log("Picked: no node association found");
      return;
    }

    // IMPORTANT: get revitID first (needed for debugging + potential direct lookup)
    const revitID = gltfJsonRef.current.nodes?.[nodeIndex]?.revitID;
    console.log("Picked nodeIndex:", nodeIndex, "revitID:", revitID);

    // Map lookup: GlobalId -> [nodeIndex]  (or sometimes RevitUniqueId -> [nodeIndex])
    let globalId = "";
    const m = ifcMapRef.current;

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

    if (!globalId) {
      const firstKey = m ? Object.keys(m)[0] : "";
      console.log("Picked: no GlobalId found for nodeIndex", nodeIndex);
      console.log("Map first key sample:", firstKey);
      return;
    }

    

    //const intId = revitMapRef.current?.[revitID]?.intId;
    let intId: number | undefined = undefined;
    if (revitID && revitMapRef.current) {
      const hit = Object.values(revitMapRef.current).find(
        (v: any) => v?.uniqueId === revitID
      ) as any;
      intId = hit?.intId;
    }

    setPickedId(String(revitID ?? ""));
    //setPickedId(String(intId ?? ""));

    console.log("Picked node:", nodeIndex, "revitID:", revitID, "revitIntID:", intId, "globalId:", globalId);
  }
  */