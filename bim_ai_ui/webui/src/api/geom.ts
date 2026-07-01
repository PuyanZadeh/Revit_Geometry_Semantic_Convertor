import api from "../api";
export async function runGeom(payload = {}) {
  const res = await api.post("/geom", payload);
  return res.data;
}
