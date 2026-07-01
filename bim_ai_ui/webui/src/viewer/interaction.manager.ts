// interaction.manager.ts
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls";


export function pickRevitId_interaction(
      e: React.MouseEvent<HTMLDivElement>,
  containerRef: React.RefObject<HTMLDivElement | null>,
  cameraRef: React.RefObject<THREE.PerspectiveCamera | null>,
  modelRef: React.RefObject<THREE.Object3D | null>,
  assocRef: React.RefObject<Map<any, any> | null>,
  controlsRef: React.RefObject<OrbitControls | null>
) {
  if (
    !cameraRef.current ||
    !modelRef.current ||
    !containerRef.current ||
    !assocRef.current
  ) {
    return { nodeIndex: null };
  }

  const rect = containerRef.current.getBoundingClientRect();
  const mouse = new THREE.Vector2(
    ((e.clientX - rect.left) / rect.width) * 2 - 1,
    -(((e.clientY - rect.top) / rect.height) * 2 - 1)
  );

  const raycaster = new THREE.Raycaster();
  raycaster.setFromCamera(mouse, cameraRef.current);

  const hits = raycaster.intersectObject(modelRef.current, true);
  if (!hits.length) return { nodeIndex: null };

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

    o = o.parent;
  }

  return { nodeIndex };
}