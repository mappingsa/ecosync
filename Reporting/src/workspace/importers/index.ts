// Read whatever files the person picked into map layers (longitude / latitude).
//
//   .zip  - Shapefile(s) inside, or a KML / GPX / GeoJSON / CSV
//   .shp + .dbf + .prj (+ .cpg) chosen together
//   .kml / .kmz, .gpx, .geojson / .json, .csv

import type { Geometry, Position } from "../../data/types";
import { NeedsZone, readCsv, readGeoJson, readGpx, readKml, type ImportLayer } from "./formats";
import { readShapefile } from "./shapefile";
import { unzip } from "./zip";

export { NeedsZone, type ImportLayer };

export interface InFile {
  name: string;
  data: Uint8Array;
}

export const ACCEPT = ".zip,.shp,.dbf,.prj,.cpg,.shx,.kml,.kmz,.gpx,.geojson,.json,.csv";
const MAX_FEATURES = 50000;

const ext = (n: string) => n.toLowerCase().match(/\.([^.\\/]+)$/)?.[1] ?? "";
const stem = (n: string) => n.replace(/^.*[\\/]/, "").replace(/\.[^.]+$/, "");
const utf8 = (b: Uint8Array) => new TextDecoder("utf-8").decode(b).replace(/^﻿/, "");

export async function readFiles(files: InFile[], opts: { zone?: number } = {}): Promise<ImportLayer[]> {
  // open zips (a KMZ is a zip with a .kml inside)
  const all: InFile[] = [];
  for (const f of files) {
    const e = ext(f.name);
    if (e === "zip" || e === "kmz") {
      const inner = await unzip(f.data);
      for (const [name, data] of inner) {
        // a KMZ's main file is often doc.kml: name it after the KMZ instead
        all.push({ name: e === "kmz" && ext(name) === "kml" ? `${stem(f.name)}.kml` : name, data });
      }
    } else all.push(f);
  }

  const layers: ImportLayer[] = [];
  const byStem = new Map<string, Map<string, InFile>>(); // shapefile parts, by name without extension
  for (const f of all) {
    const e = ext(f.name);
    if (["shp", "dbf", "prj", "cpg", "shx"].includes(e)) {
      const k = f.name.replace(/\.[^.]+$/, "").toLowerCase();
      if (!byStem.has(k)) byStem.set(k, new Map());
      byStem.get(k)!.set(e, f);
    }
  }
  for (const parts of byStem.values()) {
    const shp = parts.get("shp");
    if (!shp) continue; // a stray .dbf etc. on its own
    layers.push(readShapefile(stem(shp.name), shp.data, parts.get("dbf")?.data,
      parts.has("prj") ? utf8(parts.get("prj")!.data) : undefined, parts.has("cpg") ? utf8(parts.get("cpg")!.data) : undefined));
  }
  for (const f of all) {
    const e = ext(f.name);
    if (e === "kml") layers.push(readKml(utf8(f.data), f.name));
    else if (e === "gpx") layers.push(readGpx(utf8(f.data), f.name));
    else if (e === "geojson" || e === "json") layers.push(readGeoJson(utf8(f.data), f.name));
    else if (e === "csv") layers.push(readCsv(utf8(f.data), f.name, opts.zone));
  }
  if (!layers.length) {
    throw new Error(byStem.size
      ? "The .shp file is missing: choose the .shp, .dbf and .prj together, or zip them."
      : "Nothing here to import. Choose a zipped Shapefile (or its .shp, .dbf and .prj), KML or KMZ, GPX, GeoJSON or CSV.");
  }

  for (const l of layers) {
    if (l.features.length > MAX_FEATURES) throw new Error(`${l.name} has ${l.features.length.toLocaleString()} features; the most one layer can hold is ${MAX_FEATURES.toLocaleString()}. Clip it to the area you need first.`);
    const bad = l.features.filter((f) => !inRange(f.geometry)).length;
    if (bad) {
      throw new Error(`${l.name}: ${bad === l.features.length ? "the" : `${bad} of the`} coordinates aren't valid latitude / longitude, so the file's projection wasn't read correctly (${l.projection}). Save it as GDA2020 or WGS84 and try again.`);
    }
    if (!l.features.length && !l.warnings.length) l.warnings.push("No points, lines or areas were found in it.");
  }
  return layers;
}

function inRange(g: Geometry | null): boolean {
  if (!g) return false;
  const ok = (p: Position) => Math.abs(p[0]) <= 180 && Math.abs(p[1]) <= 90;
  switch (g.type) {
    case "Point": return ok(g.coordinates);
    case "LineString": return g.coordinates.every(ok);
    case "MultiLineString": case "Polygon": return g.coordinates.every((r) => r.every(ok));
    case "MultiPolygon": return g.coordinates.every((p) => p.every((r) => r.every(ok)));
  }
}

/** Geometry -> EWKT for saving (the column takes any type). */
export function geomToEwkt(g: Geometry): string {
  const pt = (p: Position) => `${p[0]} ${p[1]}`;
  const path = (r: Position[]) => `(${r.map(pt).join(",")})`;
  switch (g.type) {
    case "Point": return `SRID=4326;POINT(${pt(g.coordinates)})`;
    case "LineString": return `SRID=4326;LINESTRING${path(g.coordinates)}`;
    case "MultiLineString": return `SRID=4326;MULTILINESTRING(${g.coordinates.map(path).join(",")})`;
    case "Polygon": return `SRID=4326;POLYGON(${g.coordinates.map(path).join(",")})`;
    case "MultiPolygon": return `SRID=4326;MULTIPOLYGON(${g.coordinates.map((p) => `(${p.map(path).join(",")})`).join(",")})`;
  }
}

/** "12 areas, 3 lines, 40 points" */
export function describe(l: ImportLayer): string {
  const n = { area: 0, line: 0, point: 0 };
  for (const f of l.features) {
    const t = f.geometry?.type ?? "";
    if (t.includes("Polygon")) n.area++;
    else if (t.includes("Line")) n.line++;
    else if (t === "Point") n.point++;
  }
  const part = (k: number, one: string) => (k ? `${k.toLocaleString()} ${one}${k === 1 ? "" : "s"}` : "");
  return [part(n.area, "area"), part(n.line, "line"), part(n.point, "point")].filter(Boolean).join(", ") || "nothing";
}
