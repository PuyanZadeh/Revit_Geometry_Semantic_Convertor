import axios from "axios";

const api = axios.create({
  //baseURL: "https://bim-ai.bimbc.com/api",
  //baseURL: "https://127.0.0.1:8080/api",
  //baseURL: "http://127.0.0.1:8080/api",
  baseURL: "http://192.168.0.150:8080/api",
  headers: { "Content-Type": "application/json" },
});

export default api;
