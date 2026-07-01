import api from "../api";
export async function runModeledit(payload = {}) {
  const res = await api.post("/modeledit", payload);
  return res.data;
}
