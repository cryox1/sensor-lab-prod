const API_PORT = process.env.NEXT_PUBLIC_API_PORT || "8000";
const WRITE_TOKEN = process.env.NEXT_PUBLIC_API_WRITE_TOKEN || "";

function envOrEmpty(value) {
  return typeof value === "string" && value.length > 0 ? value : null;
}

export function getApiBase() {
  const override = envOrEmpty(process.env.NEXT_PUBLIC_API_BASE);
  if (override) return override;
  if (typeof window === "undefined") return null;
  return `${window.location.protocol}//${window.location.hostname}:${API_PORT}`;
}

export function getWsUrl() {
  const override = envOrEmpty(process.env.NEXT_PUBLIC_WS_URL);
  if (override) return override;
  if (typeof window === "undefined") return null;
  const scheme = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${window.location.hostname}:${API_PORT}/ws/live`;
}

// Headers for any mutating fetch. Adds X-API-Token only when the deploy
// configured a write token; otherwise returns just content-type.
export function writeHeaders() {
  const h = { "content-type": "application/json" };
  if (WRITE_TOKEN) h["X-API-Token"] = WRITE_TOKEN;
  return h;
}
