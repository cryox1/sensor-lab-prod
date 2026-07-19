"use client";
import { displayNameFor } from "../_lib/displayName";
import { offsetFor, applyOffset } from "../_lib/offsets";
import NameEditor from "./NameEditor";

function formatTs(iso) {
  if (!iso) return "—";
  return new Date(iso).toLocaleString();
}

// One sensor tile: device id + name (inline-editable), live climate / air
// readouts (display offsets applied), last-seen and a history link, plus a
// corner ✕ to hide it from the dashboard. Used on the startpage both inside
// group sections and the "Ungrouped" section.
export default function SensorCard({ id, live, meta, api, devicesKey, offsets, onHide }) {
  const l = live;
  const name = displayNameFor(meta ?? { device_id: id });
  const hasAlias = !!(meta && meta.display_name);
  const hasClimate = l && l.temp_c != null;
  const hasAir = l && l.eco2_ppm != null;

  return (
    <div
      style={{
        background: "#161b22",
        border: "1px solid #2a313c",
        borderRadius: 12,
        padding: 20,
        position: "relative",
      }}
    >
      <button
        type="button"
        onClick={() => onHide(id)}
        title="hide from dashboard"
        style={{
          position: "absolute",
          top: 8,
          right: 10,
          background: "none",
          border: "none",
          color: "#666",
          cursor: "pointer",
          fontSize: 14,
          padding: 4,
        }}
      >
        ✕
      </button>
      <div
        style={{
          fontSize: 11,
          opacity: 0.5,
          fontFamily: "monospace",
        }}
      >
        {id}
      </div>
      <div
        style={{
          display: "flex",
          alignItems: "center",
          gap: 8,
          marginTop: 2,
        }}
      >
        <div style={{ fontSize: 18, fontWeight: 600 }}>{name}</div>
        {api && (
          <NameEditor
            api={api}
            deviceId={id}
            currentName={hasAlias ? meta.display_name : ""}
            devicesKey={devicesKey}
          />
        )}
      </div>

      {hasClimate && (
        <>
          <div style={{ marginTop: 16, fontSize: 40, fontWeight: 700 }}>
            {applyOffset(l.temp_c, offsetFor(offsets, id, "temp_c")).toFixed(1)}°C
          </div>
          {l.humidity != null && (
            <div style={{ fontSize: 22, opacity: 0.85 }}>
              {Math.round(applyOffset(l.humidity, offsetFor(offsets, id, "humidity")))} % RH
            </div>
          )}
          {l.heat_index_c != null && (
            <div style={{ marginTop: 8, fontSize: 13, opacity: 0.6 }}>
              feels like {applyOffset(l.heat_index_c, offsetFor(offsets, id, "heat_index_c")).toFixed(1)}°C
            </div>
          )}
        </>
      )}

      {hasAir && (
        <>
          <div style={{ marginTop: 16, fontSize: 32, fontWeight: 700 }}>
            {Math.round(applyOffset(l.eco2_ppm, offsetFor(offsets, id, "eco2_ppm")))} <span style={{ fontSize: 16, opacity: 0.6 }}>ppm eCO₂</span>
          </div>
          {l.tvoc_ppb != null && (
            <div style={{ fontSize: 18, opacity: 0.85 }}>
              {Math.round(applyOffset(l.tvoc_ppb, offsetFor(offsets, id, "tvoc_ppb")))} ppb TVOC
            </div>
          )}
          {l.aqi != null && (
            <div style={{ fontSize: 18, opacity: 0.85 }}>
              AQI {applyOffset(l.aqi, offsetFor(offsets, id, "aqi"))}/5
            </div>
          )}
        </>
      )}

      {!hasClimate && !hasAir && l && l.batt_v == null && l.pressure_hpa == null && l.gas_kohm == null && l.lightning_count == null && l.iaq == null && (
        <div style={{ marginTop: 16, fontSize: 22, opacity: 0.5 }}>—</div>
      )}

      {l && l.iaq != null && (
        <div style={{ marginTop: 12, fontSize: 13, opacity: 0.6 }}>
          IAQ {Math.round(Number(l.iaq))}
          {l.iaq_acc != null && Number(l.iaq_acc) < 3
            ? ` (calibrating ${l.iaq_acc}/3)`
            : ""}
          {l.co2_eq_ppm != null ? ` · CO₂eq ${Math.round(Number(l.co2_eq_ppm))} ppm` : ""}
        </div>
      )}

      {l && l.pressure_hpa != null && (
        <div style={{ marginTop: 12, fontSize: 13, opacity: 0.6 }}>
          pressure {Number(l.pressure_hpa).toFixed(1)} hPa
        </div>
      )}

      {l && l.gas_kohm != null && (
        <div style={{ marginTop: 12, fontSize: 13, opacity: 0.6 }}>
          gas {Math.round(Number(l.gas_kohm))} kΩ
        </div>
      )}

      {l && l.lightning_count != null && (
        <div style={{ marginTop: 12, fontSize: 13, opacity: 0.6 }}>
          lightning {Math.round(Number(l.lightning_count))} strikes
          {l.lightning_km != null ? `, storm ~${Math.round(Number(l.lightning_km))} km` : ""}
        </div>
      )}

      {l && l.batt_v != null && (
        <div style={{ marginTop: 12, fontSize: 13, opacity: 0.6 }}>
          battery {Number(l.batt_v).toFixed(2)} V
        </div>
      )}

      <div
        style={{
          marginTop: 16,
          fontSize: 12,
          opacity: 0.5,
          borderTop: "1px solid #2a313c",
          paddingTop: 8,
        }}
      >
        last seen: {formatTs(meta?.last_seen)}
      </div>
      <a
        href={`/history/${id}`}
        style={{
          display: "inline-block",
          marginTop: 12,
          color: "#58a6ff",
          fontSize: 13,
        }}
      >
        history →
      </a>
    </div>
  );
}
