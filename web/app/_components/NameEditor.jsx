"use client";
import { useEffect, useRef, useState } from "react";
import { mutate } from "swr";
import { writeHeaders } from "../_lib/api";

export default function NameEditor({ api, deviceId, currentName, devicesKey }) {
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(currentName ?? "");
  const [saving, setSaving] = useState(false);
  const inputRef = useRef(null);

  useEffect(() => {
    if (editing) {
      setValue(currentName ?? "");
      requestAnimationFrame(() => inputRef.current?.select());
    }
  }, [editing, currentName]);

  async function save() {
    setSaving(true);
    try {
      await fetch(`${api}/devices/${encodeURIComponent(deviceId)}/display-name`, {
        method: "PUT",
        headers: writeHeaders(),
        body: JSON.stringify({ display_name: value }),
      });
      if (devicesKey) mutate(devicesKey);
      setEditing(false);
    } finally {
      setSaving(false);
    }
  }

  if (!editing) {
    return (
      <button
        type="button"
        onClick={() => setEditing(true)}
        title="rename"
        style={{
          background: "none",
          border: "none",
          color: "#58a6ff",
          cursor: "pointer",
          fontSize: 13,
          padding: 0,
        }}
      >
        ✎
      </button>
    );
  }

  return (
    <span style={{ display: "inline-flex", gap: 6, alignItems: "center" }}>
      <input
        ref={inputRef}
        value={value}
        onChange={(e) => setValue(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter") save();
          if (e.key === "Escape") setEditing(false);
        }}
        maxLength={64}
        disabled={saving}
        placeholder={deviceId}
        style={{
          background: "#0d1117",
          color: "#e6edf3",
          border: "1px solid #2a313c",
          borderRadius: 6,
          padding: "4px 8px",
          fontSize: 16,
          width: 160,
        }}
      />
      <button
        type="button"
        onClick={save}
        disabled={saving}
        style={{
          background: "none",
          border: "none",
          color: "#58a6ff",
          cursor: "pointer",
          fontSize: 13,
          padding: 0,
        }}
      >
        save
      </button>
      <button
        type="button"
        onClick={() => setEditing(false)}
        disabled={saving}
        style={{
          background: "none",
          border: "none",
          color: "#999",
          cursor: "pointer",
          fontSize: 13,
          padding: 0,
        }}
      >
        cancel
      </button>
    </span>
  );
}
