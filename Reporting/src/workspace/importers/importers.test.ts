import { test } from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { describe as describeLayer, geomToEwkt, NeedsZone, readFiles } from "./index";
import { utmZone, SA_LAMBERT } from "./proj";
import { parseXml, text, findAll } from "./xml";

// jszip only builds the test zips; the app itself unzips without it
const req = createRequire(import.meta.url);
const JSZip: any = (() => { for (const m of ["jszip", "/opt/npm-tools/node_modules/jszip"]) { try { return req(m); } catch { /* next */ } } return null; })();
const needsZip = { skip: JSZip ? false : "jszip isn't installed (only needed to build test zips)" };
const enc = new TextEncoder();
const file = (name: string, s: string | Uint8Array) => ({ name, data: typeof s === "string" ? enc.encode(s) : s });
const near = (a: number, b: number, tol = 1e-6) => Math.abs(a - b) < tol;

// --- a tiny shapefile writer, just for tests ---
function shp(type: number, shapes: number[][][]): Uint8Array {
  const recs = shapes.map((parts) => {
    if (type === 1) { const b = new DataView(new ArrayBuffer(20)); b.setInt32(0, 1, true); b.setFloat64(4, parts[0][0], true); b.setFloat64(12, parts[0][1], true); return new Uint8Array(b.buffer); }
    const pts = parts.flat();
    const b = new DataView(new ArrayBuffer(44 + parts.length * 4 + pts.length * 16));
    b.setInt32(0, type, true); b.setInt32(36, parts.length, true); b.setInt32(40, pts.length, true);
    let s = 0; parts.forEach((p, i) => { b.setInt32(44 + i * 4, s, true); s += p.length; });
    pts.forEach((p, i) => { b.setFloat64(44 + parts.length * 4 + i * 16, p[0], true); b.setFloat64(52 + parts.length * 4 + i * 16, p[1], true); });
    return new Uint8Array(b.buffer);
  });
  const len = 100 + recs.reduce((s, r) => s + 8 + r.length, 0);
  const out = new Uint8Array(len); const dv = new DataView(out.buffer);
  dv.setInt32(0, 9994, false); dv.setInt32(24, len / 2, false); dv.setInt32(28, 1000, true); dv.setInt32(32, type, true);
  let p = 100;
  recs.forEach((r, i) => { dv.setInt32(p, i + 1, false); dv.setInt32(p + 4, r.length / 2, false); out.set(r, p + 8); p += 8 + r.length; });
  return out;
}
function dbf(fields: [string, string, number][], rows: string[][], encode = (s: string) => enc.encode(s)): Uint8Array {
  const recLen = 1 + fields.reduce((s, f) => s + f[2], 0), headLen = 32 + fields.length * 32 + 1;
  const out = new Uint8Array(headLen + rows.length * recLen + 1); const dv = new DataView(out.buffer);
  out[0] = 3; dv.setUint32(4, rows.length, true); dv.setUint16(8, headLen, true); dv.setUint16(10, recLen, true);
  fields.forEach(([n, t, l], i) => { out.set(enc.encode(n), 32 + i * 32); out[32 + i * 32 + 11] = t.charCodeAt(0); out[32 + i * 32 + 16] = l; });
  out[headLen - 1] = 0x0d;
  rows.forEach((r, i) => {
    const base = headLen + i * recLen; out[base] = 0x20; let at = base + 1;
    fields.forEach(([, , l], k) => { const b = encode(r[k].padEnd(l).slice(0, l)); out.set(b.subarray(0, l), at); at += l; });
  });
  return out;
}
const MGA54 = `PROJCS["GDA2020_MGA_Zone_54",GEOGCS["GCS_GDA2020",DATUM["D_GDA2020",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",500000.0],PARAMETER["False_Northing",10000000.0],PARAMETER["Central_Meridian",141.0],PARAMETER["Scale_Factor",0.9996],PARAMETER["Latitude_Of_Origin",0.0],UNIT["Meter",1.0]]`;
const z54 = utmZone(54);
const E = (lon: number, lat: number) => z54.forward(lon, lat);
// a 2 km block with a hole (outer clockwise, hole anticlockwise), and a second block
const outer = [E(140.60, -34.30), E(140.62, -34.30), E(140.62, -34.32), E(140.60, -34.32), E(140.60, -34.30)].map(([x, y]) => [x, y]);
// that ring runs E then S then W then N: clockwise with y up? (+x, then -y) yes
const hole = [E(140.605, -34.305), E(140.605, -34.31), E(140.61, -34.31), E(140.61, -34.305), E(140.605, -34.305)];
const block2 = [E(140.70, -34.30), E(140.71, -34.30), E(140.71, -34.31), E(140.70, -34.31), E(140.70, -34.30)];

test("zipped Shapefile in MGA zone 54: projection, holes, attributes, dates", needsZip, async () => {
  const zip = new JSZip();
  zip.file("Property Boundary.shp", shp(5, [[outer, hole], [block2]]));
  zip.file("Property Boundary.dbf", dbf([["NAME", "C", 20], ["HA", "N", 8], ["SURVEYED", "D", 8]], [["Home paddock", "400.5", "20261118"], ["Bore block", "", "        "]]));
  zip.file("Property Boundary.prj", MGA54);
  zip.file("__MACOSX/._Property Boundary.shp", "junk");
  const data = new Uint8Array(await zip.generateAsync({ type: "uint8array", compression: "DEFLATE" }));
  const [l] = await readFiles([file("Boundaries.zip", data)]);
  assert.equal(l.name, "Property Boundary");
  assert.match(l.projection, /MGA Zone 54/i);
  assert.equal(l.features.length, 2);
  const g = l.features[0].geometry!;
  assert.equal(g.type, "Polygon");
  assert.equal((g as any).coordinates.length, 2, "the hole stays with its outer ring");
  const c = (g as any).coordinates[0][0];
  assert.ok(near(c[0], 140.60) && near(c[1], -34.30), `back to lon/lat: ${c}`);
  assert.deepEqual(l.features[0].properties, { NAME: "Home paddock", HA: 400.5, SURVEYED: "2026-11-18" });
  assert.deepEqual(l.features[1].properties, { NAME: "Bore block", HA: null, SURVEYED: null });
  assert.equal(describeLayer(l), "2 areas");
});

test("loose .shp/.dbf/.prj chosen together; lines; Windows text encoding", async () => {
  const latin1 = (s: string) => Uint8Array.from([...s].map((ch) => ch.charCodeAt(0)));
  const [l] = await readFiles([
    file("tracks.shp", shp(3, [[[ [140.6, -34.3], [140.61, -34.31] ]], [[ [140.7, -34.3], [140.71, -34.3] ], [ [140.72, -34.3], [140.73, -34.3] ]]])),
    file("tracks.dbf", dbf([["NOTE", "C", 12]], [["Café track"], ["Gate"]], latin1)),
  ]);
  assert.equal(l.features[0].geometry!.type, "LineString");
  assert.equal(l.features[1].geometry!.type, "MultiLineString");
  assert.equal(l.features[0].properties.NOTE, "Café track");
  assert.ok(l.warnings.some((w) => /No \.prj/.test(w)));
});

test("a projected shapefile without .prj is refused with a clear reason", async () => {
  await assert.rejects(readFiles([file("x.shp", shp(1, [[E(140.6, -34.3)]]))]), /no \.prj file/);
});

test("SA Lambert shapefile", async () => {
  const sal = `PROJCS["GDA_1994_South_Australia_Lambert",GEOGCS["GCS_GDA_1994"],PROJECTION["Lambert_Conformal_Conic"],PARAMETER["False_Easting",1000000.0],PARAMETER["False_Northing",2000000.0],PARAMETER["Central_Meridian",135.0],PARAMETER["Standard_Parallel_1",-28.0],PARAMETER["Standard_Parallel_2",-36.0],PARAMETER["Latitude_Of_Origin",-32.0],UNIT["Meter",1.0]]`;
  const [l] = await readFiles([file("p.shp", shp(1, [[SA_LAMBERT.forward(138.6, -34.9)]])), file("p.prj", sal)]);
  const c = (l.features[0].geometry as any).coordinates;
  assert.ok(near(c[0], 138.6) && near(c[1], -34.9));
});

const KML = `<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2" xmlns:gx="http://www.google.com/kml/ext/2.2"><Document><name>Riverland targets</name>
<Style id="s"><LineStyle><color>ff0000ff</color></LineStyle></Style>
<Folder><name>Boxthorn</name>
 <Placemark><name>Patch A &amp; B</name><description><![CDATA[<b>Dense</b><br/>near gate]]></description>
  <ExtendedData><Data name="priority"><value>1</value></Data><SchemaData schemaUrl="#x"><SimpleData name="owner">Smith</SimpleData></SchemaData></ExtendedData>
  <Polygon><outerBoundaryIs><LinearRing><coordinates>140.6,-34.3,0 140.61,-34.3,0 140.61,-34.31,0 140.6,-34.31,0 140.6,-34.3,0</coordinates></LinearRing></outerBoundaryIs>
  <innerBoundaryIs><LinearRing><coordinates>140.602,-34.302 140.603,-34.302 140.603,-34.303 140.602,-34.302</coordinates></LinearRing></innerBoundaryIs></Polygon></Placemark>
</Folder>
<Placemark><name>Fence</name><MultiGeometry><LineString><coordinates>140.6,-34.3 140.7,-34.3</coordinates></LineString><LineString><coordinates>140.7,-34.3 140.7,-34.4</coordinates></LineString></MultiGeometry></Placemark>
<Placemark><name>Drive</name><gx:Track><when>2026-11-23T00:00:00Z</when><gx:coord>140.6 -34.3 10</gx:coord><gx:coord>140.61 -34.31 12</gx:coord></gx:Track></Placemark>
<Placemark><name>Gate</name><Point><coordinates>140.65,-34.35</coordinates></Point></Placemark>
<!-- a comment <Placemark> -->
</Document></kml>`;

test("KML: folders, holes, HTML descriptions, extended data, multi-lines, gx:Track, points", async () => {
  const [l] = await readFiles([file("targets.kml", KML)]);
  assert.equal(l.name, "Riverland targets");
  assert.equal(l.features.length, 4);
  const [poly, fence, drive, gate] = l.features;
  assert.equal(poly.geometry!.type, "Polygon");
  assert.equal((poly.geometry as any).coordinates.length, 2);
  assert.deepEqual(poly.properties, { name: "Patch A & B", description: "Dense\nnear gate", folder: "Boxthorn", priority: 1, owner: "Smith" });
  assert.equal(fence.geometry!.type, "MultiLineString");
  assert.equal(drive.geometry!.type, "LineString");
  assert.deepEqual((gate.geometry as any).coordinates, [140.65, -34.35]);
  assert.equal(describeLayer(l), "1 area, 2 lines, 1 point");
});

test("KMZ (zipped KML, named doc.kml inside) takes the KMZ's name", needsZip, async () => {
  const zip = new JSZip();
  zip.file("doc.kml", KML.replace("<name>Riverland targets</name>", ""));
  zip.file("files/icon.png", new Uint8Array([1, 2, 3]));
  const [l] = await readFiles([file("Targets 2026.kmz", new Uint8Array(await zip.generateAsync({ type: "uint8array", compression: "DEFLATE" })))]);
  assert.equal(l.name, "Targets 2026");
  assert.equal(l.features.length, 4);
});

test("GPX: waypoints, routes and tracks with two segments", async () => {
  const gpx = `<?xml version="1.0"?><gpx version="1.1" creator="x" xmlns="http://www.topografix.com/GPX/1/1">
  <metadata><name>Monday</name></metadata>
  <wpt lat="-34.3" lon="140.6"><ele>21.5</ele><time>2026-11-23T00:01:00Z</time><name>Dam</name><desc>Full</desc></wpt>
  <rte><name>Way in</name><rtept lat="-34.3" lon="140.6"/><rtept lat="-34.31" lon="140.61"/></rte>
  <trk><name>Spray run</name><trkseg><trkpt lat="-34.3" lon="140.6"><time>2026-11-23T00:00:00Z</time></trkpt><trkpt lat="-34.31" lon="140.6"/></trkseg>
  <trkseg><trkpt lat="-34.32" lon="140.6"/><trkpt lat="-34.33" lon="140.6"><time>2026-11-23T01:00:00Z</time></trkpt></trkseg></trk></gpx>`;
  const [l] = await readFiles([file("monday.gpx", gpx)]);
  assert.equal(l.name, "Monday");
  assert.deepEqual(l.features.map((f) => f.geometry!.type), ["Point", "LineString", "MultiLineString"]);
  assert.deepEqual(l.features[0].properties, { name: "Dam", description: "Full", elevation_m: 21.5, time: "2026-11-23T00:01:00Z" });
  assert.deepEqual(l.features[2].properties, { name: "Spray run", start: "2026-11-23T00:00:00Z", end: "2026-11-23T01:00:00Z" });
});

test("CSV with lat / lon, quoted commas, blank rows and a bad row", async () => {
  const csv = `﻿Site,Latitude,Longitude,Count\r\n"Gate, north",-34.3,140.6,5\r\nTank,-34.31,140.61,\r\n\r\nNo coords,,,2\r\n`;
  const [l] = await readFiles([file("sites.csv", csv)]);
  assert.equal(l.features.length, 2);
  assert.deepEqual(l.features[0].properties, { Site: "Gate, north", Count: 5 });
  assert.deepEqual((l.features[1].geometry as any).coordinates, [140.61, -34.31]);
  assert.ok(l.warnings[0].startsWith("1 feature"));
});

test("CSV with Easting / Northing asks for the zone, then reads it", async () => {
  const [e, n] = E(140.6, -34.3);
  const csv = `id;Easting;Northing\n1;${e};${n}\n`;
  await assert.rejects(readFiles([file("en.csv", csv)]), (err: unknown) => err instanceof NeedsZone);
  const [l] = await readFiles([file("en.csv", csv)], { zone: 54 });
  const c = (l.features[0].geometry as any).coordinates;
  assert.ok(near(c[0], 140.6) && near(c[1], -34.3));
});

test("GeoJSON in lat/lon, and with an MGA crs", async () => {
  const fc = { type: "FeatureCollection", features: [{ type: "Feature", properties: { a: 1 }, geometry: { type: "MultiPoint", coordinates: [[140.6, -34.3, 5], [140.7, -34.4]] } }] };
  const [l] = await readFiles([file("p.geojson", JSON.stringify(fc))]);
  assert.equal(l.features.length, 2, "multipoints become separate points");
  assert.deepEqual((l.features[0].geometry as any).coordinates, [140.6, -34.3]);
  const [x, y] = E(140.6, -34.3);
  const [m] = await readFiles([file("m.json", JSON.stringify({ type: "Point", coordinates: [x, y], crs: { type: "name", properties: { name: "urn:ogc:def:crs:EPSG::7854" } } }))]);
  const c = (m.features[0].geometry as any).coordinates;
  assert.ok(near(c[0], 140.6) && near(c[1], -34.3));
  await assert.rejects(readFiles([file("bad.geojson", JSON.stringify({ type: "Point", coordinates: [x, y] }))]), /aren't valid latitude/);
});

test("nothing usable, or a lone .dbf, gets a plain message", async () => {
  await assert.rejects(readFiles([file("notes.txt", "hi")]), /Nothing here to import/);
  await assert.rejects(readFiles([file("x.dbf", new Uint8Array(40))]), /\.shp file is missing/);
});

test("EWKT for saving", () => {
  assert.equal(geomToEwkt({ type: "Polygon", coordinates: [[[0, 0], [1, 0], [1, 1], [0, 0]]] }), "SRID=4326;POLYGON((0 0,1 0,1 1,0 0))");
  assert.equal(geomToEwkt({ type: "LineString", coordinates: [[0, 0], [1, 1]] }), "SRID=4326;LINESTRING(0 0,1 1)");
});

test("XML reader: entities, CDATA, comments, namespaces, attributes with >", () => {
  const x = parseXml(`<?xml version="1.0"?><!DOCTYPE x><a:root x="1>2"><!-- <b>no</b> --><b>&lt;&#65;&#x42;&apos;</b><b><![CDATA[<i>c</i>]]></b><c/></a:root>`);
  const bs = findAll(x, "b");
  assert.deepEqual(bs.map((b) => text(b)), ["<AB'", "<i>c</i>"]);
  assert.equal(findAll(x, "root")[0].attrs.x, "1>2");
});
