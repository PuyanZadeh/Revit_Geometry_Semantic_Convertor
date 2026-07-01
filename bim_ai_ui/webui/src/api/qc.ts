import api from "../api";
export async function runQc(payload = {}) {
  const res = await api.post("/qc", payload);
  return res.data;
}
