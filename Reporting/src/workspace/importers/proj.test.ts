import { test } from "node:test";
import assert from "node:assert/strict";
import { fromCrsName, fromEpsg, fromWkt, SA_LAMBERT, utmZone, WEB_MERCATOR } from "./proj";

const dms = (d: number, m: number, s: number) => -(Math.abs(d) + m / 60 + s / 3600);

test("MGA: Geoscience Australia's Flinders Peak worked example, to a centimetre", () => {
  const lat = dms(-37, 57, 3.7203), lon = 144 + 25 / 60 + 29.5244 / 3600;
  const z55 = utmZone(55);
  const [e, n] = z55.forward(lon, lat);
  assert.ok(Math.abs(e - 273741.2966) < 0.01, `E ${e}`);
  assert.ok(Math.abs(n - 5796489.7769) < 0.01, `N ${n}`);
  const [lon2, lat2] = z55.inverse(273741.2966, 5796489.7769);
  assert.ok(Math.abs(lon2 - lon) < 1e-7 && Math.abs(lat2 - lat) < 1e-7);
});

test("MGA zone 54 central meridian and a Riverland round trip", () => {
  const z54 = fromEpsg(7854)!;
  const [lon, lat] = z54.inverse(500000, 6200000);
  assert.ok(Math.abs(lon - 141) < 1e-9);
  const [e, n] = z54.forward(140.6, -34.3);
  const [lo, la] = z54.inverse(e, n);
  assert.ok(Math.abs(lo - 140.6) < 1e-9 && Math.abs(la + 34.3) < 1e-9);
});

test("SA Lambert: the origin, and a round trip", () => {
  const [x, y] = SA_LAMBERT.forward(135, -32);
  assert.ok(Math.abs(x - 1000000) < 1e-6 && Math.abs(y - 2000000) < 1e-6);
  const [lo, la] = SA_LAMBERT.inverse(...SA_LAMBERT.forward(140.6, -34.3));
  assert.ok(Math.abs(lo - 140.6) < 1e-9 && Math.abs(la + 34.3) < 1e-9);
});

test("Web Mercator round trip", () => {
  const [lo, la] = WEB_MERCATOR.inverse(...WEB_MERCATOR.forward(140.6, -34.3));
  assert.ok(Math.abs(lo - 140.6) < 1e-9 && Math.abs(la + 34.3) < 1e-9);
});

test(".prj files as ArcGIS and QGIS write them", () => {
  const esriMga = `PROJCS["GDA_1994_MGA_Zone_54",GEOGCS["GCS_GDA_1994",DATUM["D_GDA_1994",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",500000.0],PARAMETER["False_Northing",10000000.0],PARAMETER["Central_Meridian",141.0],PARAMETER["Scale_Factor",0.9996],PARAMETER["Latitude_Of_Origin",0.0],UNIT["Meter",1.0]]`;
  const p = fromWkt(esriMga)!;
  assert.match(p.name, /MGA Zone 54/i);
  assert.ok(Math.abs(p.inverse(500000, 6200000)[0] - 141) < 1e-9);
  const esriSaLambert = `PROJCS["GDA_1994_South_Australia_Lambert",GEOGCS["GCS_GDA_1994",DATUM["D_GDA_1994",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Lambert_Conformal_Conic"],PARAMETER["False_Easting",1000000.0],PARAMETER["False_Northing",2000000.0],PARAMETER["Central_Meridian",135.0],PARAMETER["Standard_Parallel_1",-28.0],PARAMETER["Standard_Parallel_2",-36.0],PARAMETER["Latitude_Of_Origin",-32.0],UNIT["Meter",1.0]]`;
  const l = fromWkt(esriSaLambert)!;
  const [x, y] = SA_LAMBERT.forward(140.6, -34.3);
  const [lo, la] = l.inverse(x, y);
  assert.ok(Math.abs(lo - 140.6) < 1e-9 && Math.abs(la + 34.3) < 1e-9);
  assert.equal(fromWkt(`GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]]`)!.name, "Longitude / latitude");
  assert.equal(fromWkt(`PROJCS["WGS_1984_Web_Mercator_Auxiliary_Sphere",GEOGCS["GCS_WGS_1984"],PROJECTION["Mercator_Auxiliary_Sphere"]]`), WEB_MERCATOR);
  assert.equal(fromWkt(`PROJCS["Something",PROJECTION["Albers"]]`), null);
});

test("GeoJSON crs names", () => {
  assert.match(fromCrsName("urn:ogc:def:crs:EPSG::28354")!.name, /GDA94 MGA zone 54/);
  assert.equal(fromCrsName("urn:ogc:def:crs:OGC:1.3:CRS84")!.name, "Longitude / latitude");
  assert.equal(fromCrsName("EPSG:3107"), SA_LAMBERT);
  assert.equal(fromCrsName("EPSG:2193"), null);
});
