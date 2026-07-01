import api from "../api";
export async function runLogging(payload = {}) {
  const res = await api.post("/logging", payload);
  return res.data;
}
