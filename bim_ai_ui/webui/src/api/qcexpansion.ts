import api from "../api";
export async function runQcexpansion(payload = {}) {
  const res = await api.post("/qcexpansion", payload);
  return res.data;
}
