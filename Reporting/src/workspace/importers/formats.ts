// KML, GPX, CSV and GeoJSON -> features in longitude / latitude.

import type { Attributes, Feature, Geometry, Position } from "../../data/types";
import { fromCrsName, GEOGRAPHIC, utmZone, type Projection } from "./proj";
import { findAll, kid, kids, parseXml, text, type XNode } from "./xml";

export interface ImportLayer {
  name: string;
  features: Feature[];
  projection: string;
  warnings: string[];
}

/** An import problem that needs an answer from the person (e.g. which MGA zone). */
export class NeedsZone extends Error {
  constructor(readonly file: string) {
    super(`${file} has Easting and Northing columns: choose its MGA zone.`);
  }
}

// --- shared clean-up -----------------------------------------------------------------

type Raw = { type: string; coordinates?: any; geometries?: Raw[] };

const finite = (p: any) => Array.isArray(p) && Number.isFinite(+p[0]) && Number.isFinite(+p[1]);

/**
 * Any GeoJSON-like geometry -> zero or more clean geometries in lon / lat:
 * projected, heights dropped, MultiPoint and GeometryCollection split up,
 * rings closed, and anything too short to be a shape left out.
 */
export function clean(raw: Raw | null | undefined, proj: Projection = GEOGRAPHIC): Geometry[] {
  if (!raw) return [];
  const P = (p: any): Position => {
    const [x, y] = proj.inverse(+p[0], +p[1]);
    return [Math.round(x * 1e8) / 1e8, Math.round(y * 1e8) / 1e8];
  };
  const line = (ps: any[]): Position[] | null => {
    const pts = (ps ?? []).filter(finite).map(P);
    return pts.length >= 2 ? pts : null;
  };
  const ring = (ps: any[]): Position[] | null => {
    const pts = (ps ?? []).filter(finite).map(P);
    if (pts.length && (pts[0][0] !== pts[pts.length - 1][0] || pts[0][1] !== pts[pts.length - 1][1])) pts.push(pts[0]);
    return pts.length >= 4 ? pts : null;
  };
  const poly = (rs: any[]): Position[][] | null => {
    const outer = ring(rs?.[0]);
    if (!outer) return null;
    return [outer, ...(rs.slice(1).map(ring).filter(Boolean) as Position[][])];
  };
  const c = raw.coordinates;
  switch (raw.type) {
    case "Point": return finite(c) ? [{ type: "Point", coordinates: P(c) }] : [];
    case "MultiPoint": return (c ?? []).filter(finite).map((p: any) => ({ type: "Point", coordinates: P(p) }) as Geometry);
    case "LineString": { const l = line(c); return l ? [{ type: "LineString", coordinates: l }] : []; }
    case "MultiLineString": {
      const ls = (c ?? []).map(line).filter(Boolean) as Position[][];
      return ls.length === 0 ? [] : ls.length === 1 ? [{ type: "LineString", coordinates: ls[0] }] : [{ type: "MultiLineString", coordinates: ls }];
    }
    case "Polygon": { const p = poly(c); return p ? [{ type: "Polygon", coordinates: p }] : []; }
    case "MultiPolygon": {
      const ps = (c ?? []).map(poly).filter(Boolean) as Position[][][];
      return ps.length === 0 ? [] : ps.length === 1 ? [{ type: "Polygon", coordinates: ps[0] }] : [{ type: "MultiPolygon", coordinates: ps }];
    }
    case "GeometryCollection": return (raw.geometries ?? []).flatMap((g) => clean(g, proj));
    default: return [];
  }
}

/** Features from (geometry, properties) pairs, one per clean geometry. */
export function toFeatures(items: { geometry: Raw | null; properties: Attributes }[], proj: Projection, warnings: string[]): Feature[] {
  const out: Feature[] = [];
  let dropped = 0;
  for (const it of items) {
    const gs = clean(it.geometry, proj);
    if (!gs.length) dropped++;
    for (const g of gs) out.push({ type: "Feature", geometry: g, properties: it.properties });
  }
  if (dropped) warnings.push(`${dropped} feature${dropped === 1 ? "" : "s"} had no usable shape and were left out.`);
  return out;
}

const baseName = (f: string) => f.replace(/^.*[\\/]/, "").replace(/\.[^.]+$/, "");
const stripHtml = (s: string) => s.replace(/<br\s*\/?>/gi, "\n").replace(/<[^>]+>/g, " ").replace(/&nbsp;/g, " ").replace(/[ \t]+/g, " ").replace(/ *\n\s*/g, "\n").trim();
const numOrText = (v: string) => (v !== "" && /^-?\d+(\.\d+)?$/.test(v.trim()) && v.trim().length < 16 ? Number(v) : v);

// --- KML ----------------------------------------------------------------------------------

const kmlCoords = (x: XNode | undefined): number[][] =>
  text(x).split(/\s+/).filter(Boolean).map((t) => t.split(",").map(Number));

function kmlGeoms(x: XNode): Raw[] {
  switch (x.name) {
    case "Point": return [{ type: "Point", coordinates: kmlCoords(kid(x, "coordinates"))[0] }];
    case "LineString": return [{ type: "LineString", coordinates: kmlCoords(kid(x, "coordinates")) }];
    case "LinearRing": return [{ type: "Polygon", coordinates: [kmlCoords(kid(x, "coordinates"))] }];
    case "Polygon": {
      // <outerBoundaryIs><LinearRing><coordinates>, then any <innerBoundaryIs> holes
      const ringIn = (b: XNode | undefined) => {
        const lr = b ? kid(b, "LinearRing") : undefined;
        return kmlCoords(lr ? kid(lr, "coordinates") : undefined);
      };
      const outer = ringIn(kid(x, "outerBoundaryIs"));
      const inner = kids(x, "innerBoundaryIs").map(ringIn);
      return [{ type: "Polygon", coordinates: [outer, ...inner] }];
    }
    case "Track": // gx:Track: <gx:coord>lon lat alt</gx:coord>
      return [{ type: "LineString", coordinates: kids(x, "coord").map((c) => text(c).split(/\s+/).map(Number)) }];
    case "MultiGeometry":
    case "MultiTrack":
      return kids(x).flatMap(kmlGeoms);
    default: return [];
  }
}

/** Merge a Placemark's parts: all lines -> MultiLineString, all areas -> MultiPolygon, else a collection. */
function merge(parts: Raw[]): Raw | null {
  if (parts.length === 0) return null;
  if (parts.length === 1) return parts[0];
  if (parts.every((p) => p.type === "LineString")) return { type: "MultiLineString", coordinates: parts.map((p) => p.coordinates) };
  if (parts.every((p) => p.type === "Polygon")) return { type: "MultiPolygon", coordinates: parts.map((p) => p.coordinates) };
  return { type: "GeometryCollection", geometries: parts };
}

export function readKml(textIn: string, fileName: string): ImportLayer {
  const root = parseXml(textIn);
  const warnings: string[] = [];
  const items: { geometry: Raw | null; properties: Attributes }[] = [];
  const doc = findAll(root, "Document")[0];
  const walk = (x: XNode, folder: string) => {
    for (const c of kids(x)) {
      if (c.name === "Folder" || c.name === "Document") {
        const n = text(kid(c, "name"));
        walk(c, c.name === "Folder" && n ? (folder ? `${folder} / ${n}` : n) : folder);
      } else if (c.name === "Placemark") {
        const props: Attributes = {};
        const name = text(kid(c, "name"));
        if (name) props.name = name;
        const desc = stripHtml(text(kid(c, "description")));
        if (desc) props.description = desc;
        if (folder) props.folder = folder;
        const ext = kid(c, "ExtendedData");
        if (ext) {
          for (const d of findAll(ext, "Data")) if (d.attrs.name) props[d.attrs.name] = numOrText(text(kid(d, "value")));
          for (const d of findAll(ext, "SimpleData")) if (d.attrs.name) props[d.attrs.name] = numOrText(text(d));
        }
        const geoms = kids(c).flatMap(kmlGeoms);
        items.push({ geometry: merge(geoms), properties: props });
      } else if (!["Style", "StyleMap", "Schema", "ScreenOverlay", "GroundOverlay", "NetworkLink"].includes(c.name)) {
        walk(c, folder);
      }
    }
  };
  walk(root, "");
  if (findAll(root, "GroundOverlay").length) warnings.push("Image overlays in the KML were left out (only points, lines and areas come across).");
  const features = toFeatures(items, GEOGRAPHIC, warnings);
  return { name: text(kid(doc ?? root, "name")) || baseName(fileName), features, projection: GEOGRAPHIC.name, warnings };
}

// --- GPX ------------------------------------------------------------------------------------

export function readGpx(textIn: string, fileName: string): ImportLayer {
  const root = parseXml(textIn);
  const warnings: string[] = [];
  const pt = (x: XNode) => [Number(x.attrs.lon), Number(x.attrs.lat)];
  const common = (x: XNode): Attributes => {
    const p: Attributes = {};
    for (const k of ["name", "desc", "cmt", "type", "sym"]) {
      const v = text(kid(x, k));
      if (v) p[k === "desc" ? "description" : k] = v;
    }
    return p;
  };
  const items: { geometry: Raw | null; properties: Attributes }[] = [];
  for (const w of findAll(root, "wpt")) {
    const p = common(w);
    const ele = text(kid(w, "ele")), time = text(kid(w, "time"));
    if (ele) p.elevation_m = Number(ele);
    if (time) p.time = time;
    items.push({ geometry: { type: "Point", coordinates: pt(w) }, properties: p });
  }
  for (const r of findAll(root, "rte")) items.push({ geometry: { type: "LineString", coordinates: kids(r, "rtept").map(pt) }, properties: common(r) });
  for (const t of findAll(root, "trk")) {
    const segs = kids(t, "trkseg").map((s) => kids(s, "trkpt"));
    const p = common(t);
    const first = segs.flat()[0], last = segs.flat().slice(-1)[0];
    if (first && text(kid(first, "time"))) p.start = text(kid(first, "time"));
    if (last && text(kid(last, "time"))) p.end = text(kid(last, "time"));
    items.push({ geometry: { type: "MultiLineString", coordinates: segs.map((s) => s.map(pt)) }, properties: p });
  }
  const features = toFeatures(items, GEOGRAPHIC, warnings);
  const meta = kid(kid(root, "gpx") ?? root, "metadata");
  return { name: text(kid(meta ?? root, "name")) || baseName(fileName), features, projection: GEOGRAPHIC.name, warnings };
}

// --- CSV --------------------------------------------------------------------------------------

export function parseCsv(textIn: string): string[][] {
  const t = textIn.replace(/^﻿/, "");
  const firstLine = t.slice(0, t.search(/\r?\n|$/));
  const delim = [",", ";", "\t"].map((d) => [d, firstLine.split(d).length] as const).sort((a, b) => b[1] - a[1])[0][0];
  const rows: string[][] = [];
  let row: string[] = [], cell = "", q = false;
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (q) {
      if (c === '"' && t[i + 1] === '"') { cell += '"'; i++; }
      else if (c === '"') q = false;
      else cell += c;
    } else if (c === '"' && cell === "") q = true;
    else if (c === delim) { row.push(cell); cell = ""; }
    else if (c === "\n" || c === "\r") {
      if (c === "\r" && t[i + 1] === "\n") i++;
      row.push(cell); rows.push(row); row = []; cell = "";
    } else cell += c;
  }
  if (cell !== "" || row.length) { row.push(cell); rows.push(row); }
  return rows.filter((r) => r.some((v) => v.trim() !== ""));
}

const LAT = /^(lat|latitude|lat_dd|y|point_y|decimallatitude|lat_gda2020|lat_gda94)$/i;
const LON = /^(lon|long|lng|longitude|lon_dd|long_dd|x|point_x|decimallongitude|long_gda2020|long_gda94)$/i;
const EAST = /^(easting|east|e|mga_e|x_mga|easting_m|mga_easting)$/i;
const NORTH = /^(northing|north|n|mga_n|y_mga|northing_m|mga_northing)$/i;

export function readCsv(textIn: string, fileName: string, zone?: number): ImportLayer {
  const rows = parseCsv(textIn);
  if (rows.length < 2) throw new Error(`${fileName} has no rows.`);
  const head = rows[0].map((h) => h.trim());
  const col = (re: RegExp) => head.findIndex((h) => re.test(h));
  let xi = col(LON), yi = col(LAT);
  let proj: Projection = GEOGRAPHIC;
  if (xi < 0 || yi < 0) {
    const ei = col(EAST), ni = col(NORTH);
    if (ei < 0 || ni < 0) throw new Error(`${fileName}: couldn't find coordinate columns. Name them Latitude and Longitude (or Easting and Northing).`);
    if (!zone) throw new NeedsZone(fileName);
    [xi, yi, proj] = [ei, ni, utmZone(zone, true, `MGA zone ${zone}`)];
  }
  const warnings: string[] = [];
  const items = rows.slice(1).map((r) => {
    const props: Attributes = {};
    head.forEach((h, k) => { if (k !== xi && k !== yi && h) props[h] = numOrText((r[k] ?? "").trim()); });
    const x = Number((r[xi] ?? "").trim()), y = Number((r[yi] ?? "").trim());
    const ok = (r[xi] ?? "").trim() !== "" && (r[yi] ?? "").trim() !== "" && Number.isFinite(x) && Number.isFinite(y);
    return { geometry: ok ? { type: "Point", coordinates: [x, y] } : null, properties: props };
  });
  if (proj === GEOGRAPHIC && items.some((i) => i.geometry && Math.abs(i.geometry.coordinates[0]) > 180)) {
    throw new Error(`${fileName}: the Longitude column has numbers over 180 - are these Eastings? Rename the columns Easting and Northing.`);
  }
  return { name: baseName(fileName), features: toFeatures(items, proj, warnings), projection: proj.name, warnings };
}

// --- GeoJSON ------------------------------------------------------------------------------------

export function readGeoJson(textIn: string, fileName: string): ImportLayer {
  let j: any;
  try { j = JSON.parse(textIn); } catch { throw new Error(`${fileName} isn't valid GeoJSON.`); }
  const crsName = j?.crs?.properties?.name;
  const proj = crsName ? fromCrsName(String(crsName)) : GEOGRAPHIC;
  if (!proj) throw new Error(`${fileName} uses a coordinate system this can't convert (${crsName}). Save it as WGS84 / GDA2020 latitude-longitude.`);
  const feats: any[] = j?.type === "FeatureCollection" ? j.features ?? [] : j?.type === "Feature" ? [j] : j?.type ? [{ geometry: j, properties: {} }] : [];
  const warnings: string[] = [];
  const features = toFeatures(feats.map((f) => ({ geometry: f?.geometry ?? null, properties: { ...(f?.properties ?? {}) } })), proj, warnings);
  return { name: j?.name || baseName(fileName), features, projection: proj.name, warnings };
}
