import api from "../api";
export async function runClashdetection(payload = {}) {
  const res = await api.post("/clashdetection", payload);
  return res.data;
}
