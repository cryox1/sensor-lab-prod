"use client";
import { useEffect, useMemo, useState } from "react";
import useSWR, { mutate } from "swr";
import { getApiBase, writeHeaders } from "./_lib/api";
import { useOffsets } from "./_lib/offsets";
import { useLiveSocket } from "./_lib/useLiveSocket";
import ManageDevicesDrawer from "./_components/ManageDevicesDrawer";
import SensorCard from "./_components/SensorCard";

const fetcher = (url) => fetch(url).then((r) => r.json());

const headerLinkStyle = {
  color: "#58a6ff",
  fontSize: 14,
  textDecoration: "none",
  padding: "8px 14px",
  border: "1px solid #2a313c",
  borderRadius: 999,
};

export default function Page() {
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const devicesKey = api ? `${api}/devices` : null;
  const { data: devices } = useSWR(devicesKey, fetcher, {
    refreshInterval: 10000,
  });
  const groupsKey = api ? `${api}/groups` : null;
  const { data: groups } = useSWR(groupsKey, fetcher, {
    refreshInterval: 10000,
  });
  // Newest stored row per device — the fallback that keeps low-duty-cycle
  // (deep-sleep) nodes visible between their infrequent live messages.
  const latestKey = api ? `${api}/latest` : null;
  const { data: latestList } = useSWR(latestKey, fetcher, {
    refreshInterval: 15000,
  });
  const [live, setLive] = useState({});
  const [drawerOpen, setDrawerOpen] = useState(false);
  const offsets = useOffsets();

  useLiveSocket((m) => {
    // Strike broadcasts (type: "strike") carry no device_id — without this
    // guard each one would create a phantom "undefined" sensor card.
    if (!m || typeof m.device_id !== "string") return;
    setLive((prev) => ({
      ...prev,
      [m.device_id]: { ...m, received_at: Date.now() },
    }));
  });

  // Per-device reading shown on the cards: start from the DB's newest row, then
  // let a live socket message override it when it's at least as new. Live `ts` is
  // epoch seconds; the DB row's `ts` is an ISO string — normalize both to ms.
  const readings = useMemo(() => {
    const out = {};
    for (const row of latestList ?? []) out[row.device_id] = row;
    for (const [id, m] of Object.entries(live)) {
      const liveMs = (m.ts ?? 0) * 1000;
      const prevMs = out[id]?.ts ? Date.parse(out[id].ts) : 0;
      if (liveMs >= prevMs) out[id] = m;
    }
    return out;
  }, [latestList, live]);

  const hiddenSet = new Set(
    (devices ?? []).filter((d) => d.hidden).map((d) => d.device_id)
  );
  const allIds = Array.from(
    new Set([
      ...(devices?.map((d) => d.device_id) ?? []),
      ...Object.keys(readings),
    ])
  );
  const visibleIds = allIds.filter((id) => !hiddenSet.has(id));
  const hiddenCount = allIds.length - visibleIds.length;

  // group_id of each device, and the section layout: each group (already sorted
  // by name from the API) with its visible members, then everything ungrouped.
  const groupOf = useMemo(() => {
    const m = new Map();
    for (const g of groups ?? [])
      for (const id of g.device_ids) m.set(id, g.id);
    return m;
  }, [groups]);

  const visibleSet = useMemo(() => new Set(visibleIds), [visibleIds]);

  const sections = useMemo(() => {
    const list = (groups ?? [])
      .map((g) => ({
        id: g.id,
        name: g.name,
        ids: g.device_ids.filter((id) => visibleSet.has(id)),
      }))
      .filter((g) => g.ids.length > 0);
    const ungrouped = visibleIds.filter((id) => !groupOf.has(id));
    return { groups: list, ungrouped };
  }, [groups, groupOf, visibleIds, visibleSet]);

  const hasGroups = (groups?.length ?? 0) > 0;

  async function hideDevice(deviceId) {
    if (!api) return;
    await fetch(`${api}/devices/${encodeURIComponent(deviceId)}/visibility`, {
      method: "PUT",
      headers: writeHeaders(),
      body: JSON.stringify({ hidden: true }),
    });
    if (devicesKey) mutate(devicesKey);
  }

  function renderGrid(ids) {
    return (
      <div
        style={{
          display: "grid",
          gridTemplateColumns: "repeat(auto-fill, minmax(260px, 1fr))",
          gap: 16,
          marginTop: 16,
        }}
      >
        {ids.map((id) => (
          <SensorCard
            key={id}
            id={id}
            live={readings[id]}
            meta={devices?.find((d) => d.device_id === id)}
            api={api}
            devicesKey={devicesKey}
            offsets={offsets}
            onHide={hideDevice}
          />
        ))}
      </div>
    );
  }

  return (
    <main style={{ padding: "32px", maxWidth: 1000, margin: "0 auto" }}>
      <div
        style={{
          display: "flex",
          alignItems: "baseline",
          justifyContent: "space-between",
          gap: 16,
          flexWrap: "wrap",
        }}
      >
        <div>
          <h1 style={{ marginBottom: 4, marginTop: 0 }}>sensor-lab</h1>
          <p style={{ opacity: 0.6, margin: 0 }}>
            ESP8266 → MQTT → PostgreSQL
          </p>
        </div>
        <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
          <button
            type="button"
            onClick={() => setDrawerOpen(true)}
            style={{
              ...headerLinkStyle,
              background: "transparent",
              cursor: "pointer",
            }}
          >
            manage
          </button>
          <a href="/groups" style={headerLinkStyle}>
            groups
          </a>
          <a href="/settings" style={headerLinkStyle}>
            settings
          </a>
          <a href="/gps" style={headerLinkStyle}>
            gps map →
          </a>
          <a href="/overview" style={headerLinkStyle}>
            overview →
          </a>
        </div>
      </div>

      {visibleIds.length === 0 ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          {hiddenCount > 0
            ? `${hiddenCount} device${hiddenCount === 1 ? "" : "s"} hidden — open “manage” to show them.`
            : "No devices yet. Power on an ESP and watch this page."}
        </p>
      ) : (
        <div style={{ marginTop: 24, display: "flex", flexDirection: "column", gap: 8 }}>
          {sections.groups.map((g) => (
            <section key={g.id} style={{ marginBottom: 16 }}>
              <div
                style={{
                  display: "flex",
                  alignItems: "baseline",
                  gap: 12,
                  flexWrap: "wrap",
                }}
              >
                <h2 style={{ fontSize: 18, fontWeight: 600, margin: 0 }}>
                  {g.name}
                </h2>
                <a
                  href={`/groups/${g.id}`}
                  style={{ color: "#58a6ff", fontSize: 13 }}
                >
                  overview →
                </a>
              </div>
              {renderGrid(g.ids)}
            </section>
          ))}

          {sections.ungrouped.length > 0 && (
            <section>
              {hasGroups && (
                <h2
                  style={{
                    fontSize: 18,
                    fontWeight: 600,
                    margin: 0,
                    opacity: 0.7,
                  }}
                >
                  Ungrouped
                </h2>
              )}
              {renderGrid(sections.ungrouped)}
            </section>
          )}
        </div>
      )}

      {visibleIds.length > 0 && hiddenCount > 0 && (
        <p style={{ marginTop: 24, fontSize: 13, opacity: 0.7 }}>
          {hiddenCount} hidden —{" "}
          <button
            type="button"
            onClick={() => setDrawerOpen(true)}
            style={{
              background: "none",
              border: "none",
              padding: 0,
              color: "#58a6ff",
              cursor: "pointer",
              fontSize: 13,
            }}
          >
            manage
          </button>
        </p>
      )}

      <ManageDevicesDrawer
        api={api}
        devices={devices}
        devicesKey={devicesKey}
        open={drawerOpen}
        onClose={() => setDrawerOpen(false)}
      />
    </main>
  );
}
