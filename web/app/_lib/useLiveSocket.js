import { useEffect } from "react";
import { getWsUrl } from "./api";

// Reconnect with exponential backoff so the dashboard heals after an api
// restart without forcing a page reload.
export function useLiveSocket(onMessage) {
  useEffect(() => {
    const wsUrl = getWsUrl();
    if (!wsUrl) return;
    let cancelled = false;
    let ws = null;
    let backoffMs = 1000;
    let timer = null;

    const connect = () => {
      if (cancelled) return;
      ws = new WebSocket(wsUrl);
      ws.onopen = () => {
        backoffMs = 1000;
      };
      ws.onmessage = (ev) => {
        try {
          onMessage(JSON.parse(ev.data));
        } catch {}
      };
      ws.onclose = () => {
        if (cancelled) return;
        timer = setTimeout(connect, backoffMs);
        backoffMs = Math.min(backoffMs * 2, 30000);
      };
      ws.onerror = () => {
        try {
          ws.close();
        } catch {}
      };
    };
    connect();
    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
      if (ws) {
        ws.onclose = null;
        try {
          ws.close();
        } catch {}
      }
    };
    // onMessage is referenced via closure; this hook should run once per page.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
}
