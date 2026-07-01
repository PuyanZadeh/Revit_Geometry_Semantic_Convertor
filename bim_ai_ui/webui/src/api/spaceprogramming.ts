import api from "../api";
export async function runSpaceprogramming(payload = {}) {
  const res = await api.post("/spaceprogramming", payload);
  return res.data;
}
