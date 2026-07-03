"use client";
import { useMemo, useState } from "react";
import {
  DndContext,
  DragOverlay,
  PointerSensor,
  TouchSensor,
  useDraggable,
  useDroppable,
  useSensor,
  useSensors,
} from "@dnd-kit/core";
import { displayNameFor } from "../_lib/displayName";

// Drag & drop assignment board (specs/web-groups.md R1): one droppable
// container per group plus an "ungrouped" zone; every sensor is a draggable
// chip. Dropping calls onAssign(deviceId, groupId|null) — the page owns the
// mutation and its optimistic update.
export default function GroupBoard({ groups, devices, groupOf, onAssign, renderGroupHeader }) {
  const [activeId, setActiveId] = useState(null);
  // The activation distance keeps the buttons/links inside containers and the
  // chips themselves clickable — a plain click never starts a drag.
  const sensors = useSensors(
    useSensor(PointerSensor, { activationConstraint: { distance: 5 } }),
    useSensor(TouchSensor, { activationConstraint: { delay: 150, tolerance: 8 } })
  );

  const byId = useMemo(
    () => new Map(devices.map((d) => [d.device_id, d])),
    [devices]
  );
  // Membership may reference a device the /devices join no longer knows about;
  // render a bare-id chip rather than losing it.
  const deviceFor = (id) => byId.get(id) ?? { device_id: id, display_name: null };
  const ungrouped = devices.filter((d) => groupOf[d.device_id] == null);
  const activeDevice = activeId ? deviceFor(activeId) : null;

  function handleDragEnd({ active, over }) {
    setActiveId(null);
    if (!over) return; // dropped outside any zone, or drag cancelled
    const target =
      over.id === "ungrouped" ? null : Number(String(over.id).slice("group-".length));
    if (target === (groupOf[active.id] ?? null)) return; // same container: no-op
    onAssign(active.id, target);
  }

  return (
    <DndContext
      sensors={sensors}
      onDragStart={({ active }) => setActiveId(active.id)}
      onDragCancel={() => setActiveId(null)}
      onDragEnd={handleDragEnd}
    >
      {groups.length === 0 ? (
        <p style={{ opacity: 0.6, fontSize: 14 }}>no groups yet.</p>
      ) : (
        <div
          style={{
            display: "grid",
            gridTemplateColumns: "repeat(auto-fill, minmax(260px, 1fr))",
            gap: 12,
          }}
        >
          {groups.map((g) => (
            <GroupContainer key={g.id} group={g} header={renderGroupHeader(g)}>
              {g.device_ids.map((id) => (
                <SensorChip key={id} device={deviceFor(id)} activeId={activeId} />
              ))}
              {g.device_ids.length === 0 && (
                <Placeholder text="drop sensors here" />
              )}
            </GroupContainer>
          ))}
        </div>
      )}

      <UngroupedZone>
        {ungrouped.map((d) => (
          <SensorChip key={d.device_id} device={d} activeId={activeId} />
        ))}
        {ungrouped.length === 0 && (
          <Placeholder text="drop a sensor here to remove it from its group" />
        )}
      </UngroupedZone>

      <DragOverlay>
        {activeDevice ? <ChipBody device={activeDevice} overlay /> : null}
      </DragOverlay>
    </DndContext>
  );
}

function GroupContainer({ group, header, children }) {
  const { isOver, setNodeRef } = useDroppable({ id: `group-${group.id}` });
  return (
    <div
      ref={setNodeRef}
      style={{
        background: "#161b22",
        border: `1px solid ${isOver ? "#58a6ff" : "#2a313c"}`,
        borderRadius: 8,
        padding: 12,
        display: "flex",
        flexDirection: "column",
        gap: 10,
      }}
    >
      {header}
      <div style={{ display: "flex", flexWrap: "wrap", gap: 8 }}>{children}</div>
    </div>
  );
}

function UngroupedZone({ children }) {
  const { isOver, setNodeRef } = useDroppable({ id: "ungrouped" });
  return (
    <div
      ref={setNodeRef}
      style={{
        marginTop: 12,
        background: "#0d1117",
        border: `1px dashed ${isOver ? "#58a6ff" : "#2a313c"}`,
        borderRadius: 8,
        padding: 12,
        display: "flex",
        flexDirection: "column",
        gap: 10,
      }}
    >
      <span style={{ fontSize: 13, fontWeight: 600, opacity: 0.7 }}>ungrouped</span>
      <div style={{ display: "flex", flexWrap: "wrap", gap: 8 }}>{children}</div>
    </div>
  );
}

function SensorChip({ device, activeId }) {
  const { attributes, listeners, setNodeRef, isDragging } = useDraggable({
    id: device.device_id,
  });
  return (
    <div
      ref={setNodeRef}
      {...listeners}
      {...attributes}
      style={{ opacity: isDragging ? 0.35 : 1, touchAction: "none" }}
    >
      <ChipBody device={device} grabbable={activeId == null} />
    </div>
  );
}

function ChipBody({ device, overlay = false, grabbable = false }) {
  return (
    <div
      style={{
        display: "inline-flex",
        flexDirection: "column",
        background: "#0d1117",
        border: "1px solid #2a313c",
        borderRadius: 8,
        padding: "6px 12px",
        cursor: overlay ? "grabbing" : grabbable ? "grab" : "default",
        boxShadow: overlay ? "0 6px 16px rgba(0,0,0,0.5)" : "none",
        userSelect: "none",
      }}
    >
      <span style={{ fontSize: 13, fontWeight: 600 }}>{displayNameFor(device)}</span>
      <span style={{ fontSize: 11, fontFamily: "monospace", opacity: 0.5 }}>
        {device.device_id}
        {device.hidden ? " · hidden" : ""}
      </span>
    </div>
  );
}

function Placeholder({ text }) {
  return (
    <span style={{ fontSize: 12, opacity: 0.4, padding: "6px 0" }}>{text}</span>
  );
}
