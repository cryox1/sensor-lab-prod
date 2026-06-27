"use client";
import { useEffect, useState } from "react";
import { mutate } from "swr";
import { writeHeaders } from "../_lib/api";
import NameEditor from "./NameEditor";

function formatTs(iso) {
  if (!iso) return "—";
  return new Date(iso).toLocaleString();
}

export default function ManageDevicesDrawer({ api, devices, devicesKey, open, onClose }) {
  const [busy, setBusy] = useState(null);

  useEffect(() => {
    if (!open) return;
    const onKey = (e) => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [open, onClose]);

  if (!open) return null;

  async function setHidden(deviceId, hidden) {
    setBusy(deviceId);
    try {
      await fetch(`${api}/devices/${encodeURIComponent(deviceId)}/visibility`, {
        method: "PUT",
        headers: writeHeaders(),
        body: JSON.stringify({ hidden }),
      });
      if (devicesKey) mutate(devicesKey);
    } finally {
      setBusy(null);
    }
  }

  const sorted = (devices ?? []).slice().sort((a, b) => {
    if (a.hidden !== b.hidden) return a.hidden ? 1 : -1;
    return (a.display_name || a.device_id).localeCompare(
      b.display_name || b.device_id
    );
  });

  return (
    <div
      onClick={onClose}
      style={{
        position: "fixed",
        inset: 0,
        background: "rgba(0,0,0,0.5)",
        zIndex: 50,
        display: "flex",
        justifyContent: "flex-end",
      }}
    >
      <aside
        onClick={(e) => e.stopPropagation()}
        style={{
          width: "min(480px, 100%)",
          height: "100%",
          background: "#0d1117",
          borderLeft: "1px solid #2a313c",
          padding: 24,
          overflowY: "auto",
          color: "#e6edf3",
        }}
      >
        <div
          style={{
            display: "flex",
            justifyContent: "space-between",
            alignItems: "baseline",
            marginBottom: 16,
          }}
        >
          <h2 style={{ margin: 0, fontSize: 18 }}>manage devices</h2>
          <button
            type="button"
            onClick={onClose}
            style={{
              background: "none",
              border: "none",
              color: "#999",
              cursor: "pointer",
              fontSize: 18,
            }}
          >
            ✕
          </button>
        </div>

        {sorted.length === 0 ? (
          <p style={{ opacity: 0.6 }}>no devices known yet.</p>
        ) : (
          <ul style={{ listStyle: "none", padding: 0, margin: 0 }}>
            {sorted.map((d) => (
              <li
                key={d.device_id}
                style={{
                  padding: "12px 0",
                  borderBottom: "1px solid #2a313c",
                  opacity: d.hidden ? 0.55 : 1,
                }}
              >
                <div
                  style={{
                    display: "flex",
                    alignItems: "center",
                    gap: 8,
                    flexWrap: "wrap",
                  }}
                >
                  <div style={{ fontWeight: 600 }}>
                    {d.display_name || d.device_id}
                  </div>
                  <NameEditor
                    api={api}
                    deviceId={d.device_id}
                    currentName={d.display_name ?? ""}
                    devicesKey={devicesKey}
                  />
                  <span
                    style={{
                      marginLeft: "auto",
                      fontSize: 12,
                      color: d.hidden ? "#999" : "#7ee787",
                    }}
                  >
                    {d.hidden ? "hidden" : "visible"}
                  </span>
                </div>
                <div
                  style={{
                    fontSize: 11,
                    fontFamily: "monospace",
                    opacity: 0.5,
                    marginTop: 2,
                  }}
                >
                  {d.device_id} · last seen {formatTs(d.last_seen)}
                </div>
                <div style={{ marginTop: 8 }}>
                  <button
                    type="button"
                    disabled={busy === d.device_id}
                    onClick={() => setHidden(d.device_id, !d.hidden)}
                    style={{
                      background: d.hidden ? "#1f6feb" : "transparent",
                      color: d.hidden ? "#fff" : "#58a6ff",
                      border: "1px solid #2a313c",
                      borderRadius: 6,
                      padding: "4px 10px",
                      fontSize: 13,
                      cursor: "pointer",
                    }}
                  >
                    {d.hidden ? "show on dashboard" : "hide from dashboard"}
                  </button>
                </div>
              </li>
            ))}
          </ul>
        )}
        <p style={{ fontSize: 12, opacity: 0.5, marginTop: 16 }}>
          hidden devices still record telemetry — they're just removed from the
          main grid.
        </p>
      </aside>
    </div>
  );
}
