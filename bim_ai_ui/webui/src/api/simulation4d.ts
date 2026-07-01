import api from "../api";
export async function runSimulation4d(payload = {}) {
  const res = await api.post("/simulation4d", payload);
  return res.data;
}
