import { Fragment } from "react";
import type { Feature } from "../data/types";
import { areaM2, lengthM } from "../report/stats";
import { Icon } from "../ui/Icon";
import { describe, type ImportLayer } from "./importers";
import { fmtArea, fmtLength } from "./logic";

/** Colours offered for a reference layer: bright, and unlike the spray colours. */
export const REF_COLOURS = ["#FFD400", "#00B0FF", "#FF6D00", "#E040FB", "#FFFFFF", "#76FF03", "#FF1744"];

export interface PendingLayer extends ImportLayer {
  colour: string;
  include: boolean;
}

/** Checking an import before it's saved: name, colour, what was found. */
export function ImportCard(p: {
  fileLabel: string;
  layers: PendingLayer[];
  busy: string | null;
  onChange: (i: number, v: Partial<PendingLayer>) => void;
  onSave: () => void;
  onCancel: () => void;
}) {
  const n = p.layers.filter((l) => l.include && l.features.length).length;
  return (
    <section className="pcard import-card">
      <h3><Icon name="upload" size={15} />Import <span className="h-note">{p.fileLabel}</span></h3>
      {p.layers.map((l, i) => (
        <div className="imp-layer" key={i}>
          <div className="imp-row">
            {p.layers.length > 1 && <input type="checkbox" checked={l.include} onChange={(e) => p.onChange(i, { include: e.target.checked })} aria-label="Include this layer" />}
            <input className="imp-name" value={l.name} onChange={(e) => p.onChange(i, { name: e.target.value })} aria-label="Layer name" />
          </div>
          <p className="muted small">{describe(l)} · {l.projection}</p>
          <div className="swatches">
            {REF_COLOURS.map((c) => (
              <button key={c} className={`sw ${l.colour === c ? "on" : ""}`} style={{ background: c }} onClick={() => p.onChange(i, { colour: c })} aria-label={`Colour ${c}`} />
            ))}
          </div>
          {l.warnings.map((w, k) => <p key={k} className="field-note">{w}</p>)}
        </div>
      ))}
      <p className="muted small">Saved layers are shared with everyone in your organisation.</p>
      <div className="btn-row">
        <button className="primary" disabled={!!p.busy || n === 0} onClick={p.onSave}>
          {p.busy ? <><span className="spinner" />{p.busy}</> : <><Icon name="check" size={15} />Save {n === 1 ? "layer" : `${n} layers`}</>}
        </button>
        <button className="ghost" disabled={!!p.busy} onClick={p.onCancel}><Icon name="close" size={15} />Cancel</button>
      </div>
    </section>
  );
}

/** Which MGA zone a CSV of Eastings / Northings is in. */
export function ZoneCard(p: { file: string; zone: number; onZone: (z: number) => void; onRead: () => void; onCancel: () => void }) {
  return (
    <section className="pcard import-card">
      <h3><Icon name="upload" size={15} />Import <span className="h-note">{p.file}</span></h3>
      <p>This file has Easting and Northing columns. Which MGA zone are they in?</p>
      <label className="zone-row">
        <span>MGA zone</span>
        <select value={p.zone} onChange={(e) => p.onZone(Number(e.target.value))}>
          {[49, 50, 51, 52, 53, 54, 55, 56].map((z) => <option key={z} value={z}>{z}{z === 53 ? " (west of 138°E)" : z === 54 ? " (138°–144°E)" : ""}</option>)}
        </select>
      </label>
      <div className="btn-row">
        <button className="primary" onClick={p.onRead}>Read file</button>
        <button className="ghost" onClick={p.onCancel}>Cancel</button>
      </div>
    </section>
  );
}

export interface ShownRef {
  id: string;
  name: string;
  colour: string;
  count: number;
  visible: boolean;
  loading: boolean;
}

export function RefLayerList(p: {
  layers: ShownRef[];
  canEdit: boolean;
  onToggle: (id: string, v: boolean) => void;
  onZoom: (id: string) => void;
  onDelete: (id: string) => void;
}) {
  return (
    <section className="pcard">
      <h3><Icon name="layers" size={15} />Reference layers <span className="h-note">{p.layers.length || ""}</span></h3>
      {!p.layers.length && (
        <p className="muted small">{p.canEdit ? "Boundaries and target areas from a Shapefile, KML, GPX, GeoJSON or CSV: use Import on the map." : "None yet."}</p>
      )}
      <ul className="layer-list">
        {p.layers.map((l) => (
          <li key={l.id} className="ref-row">
            <label className="toggle-row">
              <input type="checkbox" checked={l.visible} onChange={(e) => p.onToggle(l.id, e.target.checked)} />
              <span className="swatch" style={{ background: l.colour }} />
              <span className="lname" title={l.name}>{l.name}</span>
              {l.loading ? <span className="spinner small-spin" /> : <span className="count">{l.count.toLocaleString()}</span>}
            </label>
            <button className="icon-btn sm" onClick={() => p.onZoom(l.id)} title="Zoom to layer" aria-label={`Zoom to ${l.name}`}><Icon name="extent" size={14} /></button>
            {p.canEdit && <button className="icon-btn sm danger-ico" onClick={() => p.onDelete(l.id)} title="Delete layer" aria-label={`Delete ${l.name}`}><Icon name="trash" size={14} /></button>}
          </li>
        ))}
      </ul>
    </section>
  );
}

/** A clicked reference feature: its layer, size and attributes. */
export function RefDetails(p: { layerName: string; colour: string; feature: Feature; onZoom: () => void; onClear: () => void }) {
  const g = p.feature.geometry;
  const size = g && g.type.includes("Polygon") ? `Area ${fmtArea(areaM2(p.feature))}` : g && g.type.includes("Line") ? `Length ${fmtLength(lengthM(p.feature))}` : "";
  const attrs = Object.entries(p.feature.properties).filter(([k, v]) => k !== "_id" && v !== null && v !== "");
  return (
    <section className="pcard selected">
      <h3><span className="swatch" style={{ background: p.colour }} />{p.layerName}</h3>
      {size && <p className="ref-size">{size}</p>}
      {attrs.length ? (
        <dl className="kv attrs">
          {attrs.map(([k, v]) => <Fragment key={k}><dt>{k}</dt><dd>{String(v)}</dd></Fragment>)}
        </dl>
      ) : <p className="muted small">No attributes.</p>}
      <div className="btn-row">
        <button className="ghost" onClick={p.onZoom}><Icon name="extent" size={15} />Zoom to</button>
        <button className="ghost" onClick={p.onClear}><Icon name="close" size={15} />Clear</button>
      </div>
    </section>
  );
}
