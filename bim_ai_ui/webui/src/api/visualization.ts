import api from "../api";
export async function runVisualization(payload = {}) {
  const res = await api.post("/visualization", payload);
  return res.data;
}
