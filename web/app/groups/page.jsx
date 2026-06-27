"use client";
import { useEffect, useMemo, useRef, useState } from "react";
import useSWR, { mutate } from "swr";
import { getApiBase, writeHeaders } from "../_lib/api";
import { displayNameFor } from "../_lib/displayName";

const fetcher = (url) => fetch(url).then((r) => r.json());

export default function GroupsPage() {
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const groupsKey = api ? `${api}/groups` : null;
  const devicesKey = api ? `${api}/devices` : null;
  const { data: groups } = useSWR(groupsKey, fetcher, { refreshInterval: 15000 });
  const { data: devices } = useSWR(devicesKey, fetcher, { refreshInterval: 15000 });

  const [newName, setNewName] = useState("");
  const [creating, setCreating] = useState(false);

  const refresh = () => {
    if (groupsKey) mutate(groupsKey);
    if (devicesKey) mutate(devicesKey);
  };

  async function createGroup() {
    const name = newName.trim();
    if (!name || creating) return;
    setCreating(true);
    try {
      await fetch(`${api}/groups`, {
        method: "POST",
        headers: writeHeaders(),
        body: JSON.stringify({ name }),
      });
      setNewName("");
      if (groupsKey) mutate(groupsKey);
    } finally {
      setCreating(false);
    }
  }

  // group_id currently assigned to each device, for the dropdowns.
  const groupOf = useMemo(() => {
    const m = {};
    for (const g of groups ?? []) for (const id of g.device_ids) m[id] = g.id;
    return m;
  }, [groups]);

  async function assignDevice(deviceId, groupId) {
    await fetch(`${api}/devices/${encodeURIComponent(deviceId)}/group`, {
      method: "PUT",
      headers: writeHeaders(),
      body: JSON.stringify({ group_id: groupId }),
    });
    refresh();
  }

  const sortedDevices = (devices ?? [])
    .slice()
    .sort((a, b) =>
      (a.display_name || a.device_id).localeCompare(b.display_name || b.device_id)
    );

  return (
    <main style={{ padding: "32px", maxWidth: 900, margin: "0 auto" }}>
      <a href="/" style={{ color: "#58a6ff", fontSize: 13 }}>
        ← back
      </a>
      <h1 style={{ margin: "8px 0 4px" }}>groups</h1>
      <p style={{ opacity: 0.6, marginTop: 4 }}>
        Organize sensors into named groups. The startpage shows each group as its
        own section, and every group has a combined overview chart. A sensor
        belongs to at most one group.
      </p>

      <section style={{ marginTop: 24 }}>
        <h2 style={{ fontSize: 16, fontWeight: 600, margin: "0 0 8px" }}>
          create a group
        </h2>
        <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
          <input
            value={newName}
            onChange={(e) => setNewName(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter") createGroup();
            }}
            maxLength={64}
            placeholder="group name"
            style={inputStyle(220)}
          />
          <button
            type="button"
            onClick={createGroup}
            disabled={creating || !newName.trim()}
            style={pillButtonStyle(newName.trim() ? "#1f6feb" : "#21262d", "#fff")}
          >
            create group
          </button>
        </div>
      </section>

      <section style={{ marginTop: 28 }}>
        <h2 style={{ fontSize: 16, fontWeight: 600, margin: "0 0 8px" }}>
          your groups
        </h2>
        {(groups?.length ?? 0) === 0 ? (
          <p style={{ opacity: 0.6, fontSize: 14 }}>no groups yet.</p>
        ) : (
          <ul style={{ listStyle: "none", padding: 0, margin: 0 }}>
            {groups.map((g) => (
              <GroupRow
                key={g.id}
                api={api}
                group={g}
                onChanged={() => {
                  if (groupsKey) mutate(groupsKey);
                }}
              />
            ))}
          </ul>
        )}
      </section>

      <section style={{ marginTop: 28 }}>
        <h2 style={{ fontSize: 16, fontWeight: 600, margin: "0 0 8px" }}>
          assign sensors
        </h2>
        {sortedDevices.length === 0 ? (
          <p style={{ opacity: 0.6, fontSize: 14 }}>no devices known yet.</p>
        ) : (
          <ul style={{ listStyle: "none", padding: 0, margin: 0 }}>
            {sortedDevices.map((d) => (
              <li
                key={d.device_id}
                style={{
                  display: "flex",
                  alignItems: "center",
                  gap: 12,
                  padding: "10px 0",
                  borderBottom: "1px solid #2a313c",
                  flexWrap: "wrap",
                }}
              >
                <div style={{ minWidth: 160 }}>
                  <div style={{ fontWeight: 600 }}>{displayNameFor(d)}</div>
                  <div
                    style={{
                      fontSize: 11,
                      fontFamily: "monospace",
                      opacity: 0.5,
                    }}
                  >
                    {d.device_id}
                    {d.hidden ? " · hidden" : ""}
                  </div>
                </div>
                <select
                  value={groupOf[d.device_id] ?? ""}
                  onChange={(e) =>
                    assignDevice(
                      d.device_id,
                      e.target.value === "" ? null : Number(e.target.value)
                    )
                  }
                  style={{
                    marginLeft: "auto",
                    background: "#0d1117",
                    color: "#e6edf3",
                    border: "1px solid #2a313c",
                    borderRadius: 6,
                    padding: "6px 10px",
                    fontSize: 14,
                  }}
                >
                  <option value="">— none —</option>
                  {(groups ?? []).map((g) => (
                    <option key={g.id} value={g.id}>
                      {g.name}
                    </option>
                  ))}
                </select>
              </li>
            ))}
          </ul>
        )}
      </section>
    </main>
  );
}

function GroupRow({ api, group, onChanged }) {
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(group.name);
  const [busy, setBusy] = useState(false);
  const inputRef = useRef(null);

  useEffect(() => {
    if (editing) {
      setValue(group.name);
      requestAnimationFrame(() => inputRef.current?.select());
    }
  }, [editing, group.name]);

  async function save() {
    const name = value.trim();
    if (!name) return;
    setBusy(true);
    try {
      await fetch(`${api}/groups/${group.id}`, {
        method: "PUT",
        headers: writeHeaders(),
        body: JSON.stringify({ name }),
      });
      setEditing(false);
      onChanged?.();
    } finally {
      setBusy(false);
    }
  }

  async function remove() {
    if (!window.confirm(`Delete group “${group.name}”? Its sensors become ungrouped.`))
      return;
    setBusy(true);
    try {
      await fetch(`${api}/groups/${group.id}`, {
        method: "DELETE",
        headers: writeHeaders(),
      });
      onChanged?.();
    } finally {
      setBusy(false);
    }
  }

  return (
    <li
      style={{
        display: "flex",
        alignItems: "center",
        gap: 10,
        padding: "10px 0",
        borderBottom: "1px solid #2a313c",
        flexWrap: "wrap",
      }}
    >
      {editing ? (
        <input
          ref={inputRef}
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") save();
            if (e.key === "Escape") setEditing(false);
          }}
          maxLength={64}
          disabled={busy}
          style={inputStyle(200)}
        />
      ) : (
        <span style={{ fontWeight: 600 }}>{group.name}</span>
      )}
      <span style={{ fontSize: 12, opacity: 0.5 }}>
        {group.device_ids.length} sensor{group.device_ids.length === 1 ? "" : "s"}
      </span>

      <span style={{ marginLeft: "auto", display: "flex", gap: 14 }}>
        {editing ? (
          <>
            <button type="button" onClick={save} disabled={busy} style={linkButton("#7ee787")}>
              save
            </button>
            <button
              type="button"
              onClick={() => setEditing(false)}
              disabled={busy}
              style={linkButton("#999")}
            >
              cancel
            </button>
          </>
        ) : (
          <>
            <a href={`/groups/${group.id}`} style={{ color: "#58a6ff", fontSize: 13 }}>
              overview →
            </a>
            <button type="button" onClick={() => setEditing(true)} style={linkButton("#58a6ff")}>
              rename
            </button>
            <button type="button" onClick={remove} disabled={busy} style={linkButton("#f85149")}>
              delete
            </button>
          </>
        )}
      </span>
    </li>
  );
}

function inputStyle(width) {
  return {
    background: "#0d1117",
    color: "#e6edf3",
    border: "1px solid #2a313c",
    borderRadius: 6,
    padding: "6px 10px",
    fontSize: 14,
    width,
  };
}

function pillButtonStyle(bg, color) {
  return {
    background: bg,
    color,
    border: "1px solid #2a313c",
    borderRadius: 999,
    padding: "6px 16px",
    fontSize: 14,
    cursor: "pointer",
  };
}

function linkButton(color) {
  return {
    background: "none",
    border: "none",
    color,
    cursor: "pointer",
    fontSize: 13,
    padding: 0,
  };
}
