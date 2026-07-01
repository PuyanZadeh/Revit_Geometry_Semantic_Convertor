import api from "../api";
export async function runSecurity(payload = {}) {
  const res = await api.post("/security", payload);
  return res.data;
}
