import api from "../api";
export async function runAiquery(payload = {}) {
  const res = await api.post("/aiquery", payload);
  return res.data;
}
