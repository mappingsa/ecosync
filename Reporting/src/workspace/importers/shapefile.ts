// Shapefile (.shp geometry, .dbf attributes, .prj projection, .cpg encoding)
// -> features in longitude / latitude. Points, lines, areas and multipoints,
// including the Z and M variants (heights and measures are dropped).

import type { Attributes } from "../../data/types";
import { toFeatures, type ImportLayer } from "./formats";
import { fromWkt, GEOGRAPHIC, type Projection } from "./proj";

type Raw = { type: string; coordinates: any };

/** Clockwise (y up) = an outer ring in a shapefile; anticlockwise = a hole. */
const clockwise = (r: number[][]) => {
  let s = 0;
  for (let i = 0; i < r.length - 1; i++) s += (r[i + 1][0] - r[i][0]) * (r[i + 1][1] + r[i][1]);
  return s > 0;
};

function inside(p: number[], ring: number[][]) {
  let c = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const [xi, yi] = ring[i], [xj, yj] = ring[j];
    if (yi > p[1] !== yj > p[1] && p[0] < ((xj - xi) * (p[1] - yi)) / (yj - yi) + xi) c = !c;
  }
  return c;
}

/** Rings -> polygons: each outer ring starts one; each hole joins the outer ring around it. */
export function ringsToPolygons(rings: number[][][]): number[][][][] {
  const polys: number[][][][] = [];
  const holes: number[][][] = [];
  for (const r of rings) (clockwise(r) ? polys.push([r]) : holes.push(r));
  if (!polys.length) return holes.map((h) => [h]); // all anticlockwise: written the other way round
  for (const h of holes) {
    const home = polys.find((p) => inside(h[0], p[0])) ?? polys[polys.length - 1];
    home.push(h);
  }
  return polys;
}

export function readShp(buf: Uint8Array): (Raw | null)[] {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf.length < 100 || dv.getInt32(0, false) !== 9994) throw new Error("This .shp file is damaged or isn't a shapefile.");
  const out: (Raw | null)[] = [];
  let p = 100;
  while (p + 8 <= buf.length) {
    const len = dv.getInt32(p + 4, false) * 2; // 16-bit words
    const c = p + 8;
    p = c + len;
    if (len < 4 || p > buf.length) { out.push(null); continue; }
    const type = dv.getInt32(c, true);
    const xy = (at: number) => [dv.getFloat64(at, true), dv.getFloat64(at + 8, true)];
    switch (type) {
      case 1: case 11: case 21: // Point, PointZ, PointM
        out.push({ type: "Point", coordinates: xy(c + 4) });
        break;
      case 8: case 18: case 28: { // MultiPoint
        const n = dv.getInt32(c + 36, true);
        out.push({ type: "MultiPoint", coordinates: Array.from({ length: n }, (_, i) => xy(c + 40 + i * 16)) });
        break;
      }
      case 3: case 13: case 23: case 5: case 15: case 25: { // PolyLine, Polygon (+Z, +M)
        const nParts = dv.getInt32(c + 36, true), nPts = dv.getInt32(c + 40, true);
        const starts = Array.from({ length: nParts }, (_, i) => dv.getInt32(c + 44 + i * 4, true));
        const pts0 = c + 44 + nParts * 4;
        const parts = starts.map((s, i) => Array.from({ length: (i + 1 < nParts ? starts[i + 1] : nPts) - s }, (_, k) => xy(pts0 + (s + k) * 16)));
        const line = type === 3 || type === 13 || type === 23;
        out.push(line ? { type: "MultiLineString", coordinates: parts } : { type: "MultiPolygon", coordinates: ringsToPolygons(parts) });
        break;
      }
      default: // 0 = null shape; 31 = MultiPatch (3D), not supported
        out.push(null);
    }
  }
  return out;
}

function decoder(cpg?: string): (b: Uint8Array) => string {
  const label = (cpg ?? "").trim().toLowerCase();
  if (/utf-?8|65001/.test(label)) return (b) => new TextDecoder("utf-8").decode(b);
  if (label && /1252|latin|8859/.test(label)) return (b) => new TextDecoder("windows-1252").decode(b);
  // no .cpg: UTF-8 if it is valid UTF-8, else the old Windows default
  const utf8 = new TextDecoder("utf-8", { fatal: true }), win = new TextDecoder("windows-1252");
  return (b) => { try { return utf8.decode(b); } catch { return win.decode(b); } };
}

export function readDbf(buf: Uint8Array, cpg?: string): Attributes[] {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const count = dv.getUint32(4, true), headLen = dv.getUint16(8, true), recLen = dv.getUint16(10, true);
  const dec = decoder(cpg);
  const fields: { name: string; type: string; len: number; at: number }[] = [];
  let at = 1;
  for (let p = 32; p + 32 <= headLen && buf[p] !== 0x0d; p += 32) {
    const raw = buf.subarray(p, p + 11);
    const name = new TextDecoder("latin1").decode(raw.subarray(0, raw.indexOf(0) < 0 ? 11 : raw.indexOf(0))).trim();
    const len = buf[p + 16];
    fields.push({ name, type: String.fromCharCode(buf[p + 11]), len, at });
    at += len;
  }
  const rows: Attributes[] = [];
  for (let r = 0; r < count; r++) {
    const base = headLen + r * recLen;
    if (base + recLen > buf.length) break;
    const row: Attributes = {};
    if (buf[base] === 0x2a) { rows.push(row); continue; } // deleted: keep the slot so rows line up with shapes
    for (const f of fields) {
      const s = dec(buf.subarray(base + f.at, base + f.at + f.len)).trim();
      if (f.type === "N" || f.type === "F") row[f.name] = s === "" || /^\*+$/.test(s) ? null : Number(s);
      else if (f.type === "D") row[f.name] = /^\d{8}$/.test(s) ? `${s.slice(0, 4)}-${s.slice(4, 6)}-${s.slice(6)}` : null;
      else if (f.type === "L") row[f.name] = /^[TtYy]$/.test(s) ? true : /^[FfNn]$/.test(s) ? false : null;
      else row[f.name] = s;
    }
    rows.push(row);
  }
  return rows;
}

export function readShapefile(name: string, shp: Uint8Array, dbf?: Uint8Array, prj?: string, cpg?: string): ImportLayer {
  const warnings: string[] = [];
  const geoms = readShp(shp);
  const attrs = dbf ? readDbf(dbf, cpg) : [];
  if (!dbf) warnings.push("No .dbf file came with it, so the features have no attributes.");
  let proj: Projection | null = GEOGRAPHIC;
  if (prj) {
    proj = fromWkt(prj);
    if (!proj) throw new Error(`${name}: its projection isn't one this can convert (${prj.slice(0, 60)}…). Save it as GDA2020 / GDA94 (lat-long or MGA zone) or SA Lambert and try again.`);
  } else {
    const first = geoms.find(Boolean);
    const sample = first ? JSON.stringify(first.coordinates).match(/-?\d+(\.\d+)?/g)?.slice(0, 2).map(Number) : null;
    if (sample && (Math.abs(sample[0]) > 180 || Math.abs(sample[1]) > 90)) {
      throw new Error(`${name}: there's no .prj file, and the coordinates aren't latitude / longitude, so the projection is unknown. Include the .prj file (zip the .shp, .dbf and .prj together).`);
    }
    warnings.push("No .prj file: read as latitude / longitude.");
  }
  const features = toFeatures(geoms.map((g, i) => ({ geometry: g, properties: attrs[i] ?? {} })), proj, warnings);
  return { name, features, projection: proj.name, warnings };
}
