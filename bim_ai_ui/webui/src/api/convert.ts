import api from "../api";
export async function runConvert(formData: FormData) {
  const res = await api.post("/convert", formData, {
    headers: { "Content-Type": "multipart/form-data" },
  });
  return res.data;
}
