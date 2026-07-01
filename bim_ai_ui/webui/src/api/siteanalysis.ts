import api from "../api";
export async function runSiteanalysis(payload = {}) {
  const res = await api.post("/siteanalysis", payload);
  return res.data;
}
