import api from "../api";
export async function runSimulation5d(payload = {}) {
  const res = await api.post("/simulation5d", payload);
  return res.data;
}
