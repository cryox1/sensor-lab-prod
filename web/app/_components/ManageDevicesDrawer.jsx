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
  // Delete confirmation popup: { device, stats: {rows,...}|null, error } or null.
  const [confirmDelete, setConfirmDelete] = useState(null);

  useEffect(() => {
    if (!open) return;
    const onKey = (e) => {
      if (e.key !== "Escape") return;
      // Escape closes the delete popup first, the drawer second.
      setConfirmDelete((c) => {
        if (!c) onClose();
        return null;
      });
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

  async function openDelete(device) {
    setConfirmDelete({ device, stats: null });
    // Row count for the popup; on failure the popup still works, just
    // without the count.
    try {
      const res = await fetch(
        `${api}/devices/${encodeURIComponent(device.device_id)}/stats`
      );
      if (!res.ok) return;
      const stats = await res.json();
      setConfirmDelete((c) =>
        c && c.device.device_id === device.device_id ? { ...c, stats } : c
      );
    } catch {
      // popup shows without the count
    }
  }

  async function deleteDevice(deviceId, deleteData) {
    setBusy(deviceId);
    try {
      const res = await fetch(
        `${api}/devices/${encodeURIComponent(deviceId)}?delete_data=${deleteData}`,
        { method: "DELETE", headers: writeHeaders() }
      );
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      setConfirmDelete(null);
      if (devicesKey) mutate(devicesKey);
    } catch (err) {
      setConfirmDelete((c) =>
        c
          ? { ...c, error: `delete failed (${err.message}) — check API / write token` }
          : c
      );
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
                <div style={{ marginTop: 8, display: "flex", gap: 8 }}>
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
                  <button
                    type="button"
                    disabled={busy === d.device_id}
                    onClick={() => openDelete(d)}
                    style={{
                      background: "transparent",
                      color: "#f85149",
                      border: "1px solid #2a313c",
                      borderRadius: 6,
                      padding: "4px 10px",
                      fontSize: 13,
                      cursor: "pointer",
                    }}
                  >
                    delete
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

      {confirmDelete && (
        <DeleteDeviceDialog
          state={confirmDelete}
          busy={busy === confirmDelete.device.device_id}
          onCancel={() => setConfirmDelete(null)}
          onDelete={(deleteData) =>
            deleteDevice(confirmDelete.device.device_id, deleteData)
          }
        />
      )}
    </div>
  );
}

// Confirmation popup for deleting a device (specs/web-dashboard.md R1):
// metadata always goes; the stored readings only if the destructive option is
// chosen. Keeping readings means the device reappears — /devices is rebuilt
// from stored telemetry.
function DeleteDeviceDialog({ state, busy, onCancel, onDelete }) {
  const { device, stats, error } = state;
  const name = device.display_name || device.device_id;
  const rows = stats?.rows;

  return (
    <div
      onClick={(e) => {
        e.stopPropagation();
        onCancel();
      }}
      style={{
        position: "fixed",
        inset: 0,
        background: "rgba(0,0,0,0.6)",
        zIndex: 60,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        padding: 16,
      }}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        style={{
          width: "min(420px, 100%)",
          background: "#161b22",
          border: "1px solid #2a313c",
          borderRadius: 8,
          padding: 20,
          color: "#e6edf3",
        }}
      >
        <h3 style={{ margin: "0 0 4px", fontSize: 16 }}>delete “{name}”?</h3>
        <div style={{ fontSize: 11, fontFamily: "monospace", opacity: 0.5 }}>
          {device.device_id}
        </div>

        <p style={{ fontSize: 13, opacity: 0.8, margin: "12px 0 0" }}>
          {stats == null
            ? "counting stored readings…"
            : rows > 0
              ? `${rows.toLocaleString()} stored readings (${formatTs(stats.first_ts)} – ${formatTs(stats.last_ts)})`
              : "no stored readings."}
        </p>
        {rows > 0 && (
          <p style={{ fontSize: 12, opacity: 0.6, margin: "8px 0 0" }}>
            if you keep the readings, this device reappears in the list — it is
            rebuilt from stored telemetry. a sensor that still publishes comes
            back either way.
          </p>
        )}
        {error && (
          <p style={{ fontSize: 13, color: "#f85149", margin: "8px 0 0" }}>{error}</p>
        )}

        <div
          style={{
            display: "flex",
            gap: 8,
            marginTop: 16,
            flexWrap: "wrap",
            justifyContent: "flex-end",
          }}
        >
          <button
            type="button"
            onClick={onCancel}
            disabled={busy}
            style={dialogButton("transparent", "#e6edf3")}
          >
            cancel
          </button>
          <button
            type="button"
            onClick={() => onDelete(false)}
            disabled={busy}
            style={dialogButton("#21262d", "#e6edf3")}
          >
            delete, keep readings
          </button>
          {rows > 0 && (
            <button
              type="button"
              onClick={() => onDelete(true)}
              disabled={busy}
              style={dialogButton("#da3633", "#fff")}
            >
              delete incl. {rows.toLocaleString()} readings
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

function dialogButton(bg, color) {
  return {
    background: bg,
    color,
    border: "1px solid #2a313c",
    borderRadius: 6,
    padding: "6px 12px",
    fontSize: 13,
    cursor: "pointer",
  };
}
