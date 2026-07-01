import api from "../api";
export async function runFederation(payload = {}) {
  const res = await api.post("/federation", payload);
  return res.data;
}
