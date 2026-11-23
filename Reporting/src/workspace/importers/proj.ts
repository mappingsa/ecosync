// Map projections an imported file may arrive in, back to longitude / latitude.
//
//   Transverse Mercator: MGA zones (GDA94, GDA2020) and UTM - Krüger series
//   to the 4th order, sub-millimetre (as Geoscience Australia's own sheets).
//   Lambert Conformal Conic (2 parallels): SA Lambert and similar.
//   Web Mercator (EPSG:3857).
// GDA94, GDA2020 and WGS84 are treated as the same datum: the differences
// (up to ~1.8 m) are well inside a spray record's GPS accuracy.

export type Pt = [number, number];
export interface Projection {
  name: string;
  /** projected x, y -> longitude, latitude (degrees) */
  inverse: (x: number, y: number) => Pt;
  /** longitude, latitude -> projected x, y (for tests) */
  forward: (lon: number, lat: number) => Pt;
}

const RAD = Math.PI / 180;
const GRS80 = { a: 6378137, f: 1 / 298.257222101 };

export const GEOGRAPHIC: Projection = { name: "Longitude / latitude", inverse: (x, y) => [x, y], forward: (x, y) => [x, y] };

export const WEB_MERCATOR: Projection = {
  name: "Web Mercator",
  inverse: (x, y) => [(x / 6378137) / RAD, (2 * Math.atan(Math.exp(y / 6378137)) - Math.PI / 2) / RAD],
  forward: (lon, lat) => [lon * RAD * 6378137, Math.log(Math.tan(Math.PI / 4 + (lat * RAD) / 2)) * 6378137],
};

export function transverseMercator(o: { lon0: number; k0: number; x0: number; y0: number; name: string; lat0?: number }): Projection {
  const { a, f } = GRS80;
  const n = f / (2 - f);
  const n2 = n * n, n3 = n2 * n, n4 = n3 * n;
  const A = (a / (1 + n)) * (1 + n2 / 4 + n4 / 64);
  const alpha = [n / 2 - (2 / 3) * n2 + (5 / 16) * n3 + (41 / 180) * n4, (13 / 48) * n2 - (3 / 5) * n3 + (557 / 1440) * n4, (61 / 240) * n3 - (103 / 140) * n4, (49561 / 161280) * n4];
  const beta = [n / 2 - (2 / 3) * n2 + (37 / 96) * n3 - (1 / 360) * n4, (1 / 48) * n2 + (1 / 15) * n3 - (437 / 1440) * n4, (17 / 480) * n3 - (37 / 840) * n4, (4397 / 161280) * n4];
  const delta = [2 * n - (2 / 3) * n2 - 2 * n3 + (116 / 45) * n4, (7 / 3) * n2 - (8 / 5) * n3 - (227 / 45) * n4, (56 / 15) * n3 - (136 / 35) * n4, (4279 / 630) * n4];
  const e2n = (2 * Math.sqrt(n)) / (1 + n);
  const lon0 = o.lon0 * RAD;
  // northing of the latitude of origin (0 for MGA / UTM)
  const xiAt = (phi: number) => {
    const t = Math.sinh(Math.atanh(Math.sin(phi)) - e2n * Math.atanh(e2n * Math.sin(phi)));
    const xi1 = Math.atan(t);
    let xi = xi1;
    for (let j = 1; j <= 4; j++) xi += alpha[j - 1] * Math.sin(2 * j * xi1);
    return xi;
  };
  const m0 = o.lat0 ? o.k0 * A * xiAt(o.lat0 * RAD) : 0;

  return {
    name: o.name,
    forward(lon, lat) {
      const phi = lat * RAD, dl = lon * RAD - lon0;
      const t = Math.sinh(Math.atanh(Math.sin(phi)) - e2n * Math.atanh(e2n * Math.sin(phi)));
      const xi1 = Math.atan(t / Math.cos(dl));
      const eta1 = Math.atanh(Math.sin(dl) / Math.sqrt(1 + t * t));
      let xi = xi1, eta = eta1;
      for (let j = 1; j <= 4; j++) {
        xi += alpha[j - 1] * Math.sin(2 * j * xi1) * Math.cosh(2 * j * eta1);
        eta += alpha[j - 1] * Math.cos(2 * j * xi1) * Math.sinh(2 * j * eta1);
      }
      return [o.x0 + o.k0 * A * eta, o.y0 + o.k0 * A * xi - m0];
    },
    inverse(x, y) {
      const xi = (y - o.y0 + m0) / (o.k0 * A), eta = (x - o.x0) / (o.k0 * A);
      let xi1 = xi, eta1 = eta;
      for (let j = 1; j <= 4; j++) {
        xi1 -= beta[j - 1] * Math.sin(2 * j * xi) * Math.cosh(2 * j * eta);
        eta1 -= beta[j - 1] * Math.cos(2 * j * xi) * Math.sinh(2 * j * eta);
      }
      const chi = Math.asin(Math.sin(xi1) / Math.cosh(eta1));
      let phi = chi;
      for (let j = 1; j <= 4; j++) phi += delta[j - 1] * Math.sin(2 * j * chi);
      const lambda = lon0 + Math.atan2(Math.sinh(eta1), Math.cos(xi1));
      return [lambda / RAD, phi / RAD];
    },
  };
}

/** MGA / UTM zone, southern hemisphere unless north. */
export const utmZone = (zone: number, south = true, label = `MGA zone ${zone}`) =>
  transverseMercator({ lon0: zone * 6 - 183, k0: 0.9996, x0: 500000, y0: south ? 10000000 : 0, name: label });

export function lambertConic2SP(o: { lat1: number; lat2: number; lat0: number; lon0: number; x0: number; y0: number; name: string }): Projection {
  const { a, f } = GRS80;
  const e = Math.sqrt(f * (2 - f));
  const m = (phi: number) => Math.cos(phi) / Math.sqrt(1 - e * e * Math.sin(phi) ** 2);
  const t = (phi: number) => Math.tan(Math.PI / 4 - phi / 2) / ((1 - e * Math.sin(phi)) / (1 + e * Math.sin(phi))) ** (e / 2);
  const [p1, p2, p0] = [o.lat1 * RAD, o.lat2 * RAD, o.lat0 * RAD];
  const n = (Math.log(m(p1)) - Math.log(m(p2))) / (Math.log(t(p1)) - Math.log(t(p2)));
  const F = m(p1) / (n * t(p1) ** n);
  const rho0 = a * F * t(p0) ** n;
  const lon0 = o.lon0 * RAD;
  return {
    name: o.name,
    forward(lon, lat) {
      const rho = a * F * t(lat * RAD) ** n;
      const th = n * (lon * RAD - lon0);
      return [o.x0 + rho * Math.sin(th), o.y0 + rho0 - rho * Math.cos(th)];
    },
    inverse(x, y) {
      const dx = x - o.x0, dy = rho0 - (y - o.y0);
      const rho = Math.sign(n) * Math.sqrt(dx * dx + dy * dy);
      const th = Math.atan2(Math.sign(n) * dx, Math.sign(n) * dy);
      const tt = (rho / (a * F)) ** (1 / n);
      let phi = Math.PI / 2 - 2 * Math.atan(tt);
      for (let i = 0; i < 15; i++) {
        const s = e * Math.sin(phi);
        const next = Math.PI / 2 - 2 * Math.atan(tt * ((1 - s) / (1 + s)) ** (e / 2));
        if (Math.abs(next - phi) < 1e-12) { phi = next; break; }
        phi = next;
      }
      return [(th / n + lon0) / RAD, phi / RAD];
    },
  };
}

export const SA_LAMBERT = lambertConic2SP({ lat1: -28, lat2: -36, lat0: -32, lon0: 135, x0: 1000000, y0: 2000000, name: "SA Lambert" });

/** A projection from an EPSG code, or null if it isn't one we know. */
export function fromEpsg(code: number): Projection | null {
  if ([4326, 4283, 7844, 4939, 4979, 4167].includes(code)) return GEOGRAPHIC;
  if ([3857, 900913, 102100, 102113, 3785].includes(code)) return WEB_MERCATOR;
  if (code >= 28348 && code <= 28358) return utmZone(code - 28300, true, `GDA94 MGA zone ${code - 28300}`);
  if (code >= 7846 && code <= 7859) return utmZone(code - 7800, true, `GDA2020 MGA zone ${code - 7800}`);
  if (code >= 32701 && code <= 32760) return utmZone(code - 32700, true, `UTM zone ${code - 32700}S`);
  if (code >= 32601 && code <= 32660) return utmZone(code - 32600, false, `UTM zone ${code - 32600}N`);
  if (code === 3107 || code === 8059) return SA_LAMBERT;
  return null;
}

/** A projection from a .prj file's WKT, or null if it isn't one we can handle. */
export function fromWkt(wkt: string): Projection | null {
  const w = wkt.replace(/\s+/g, " ");
  const auth = w.match(/AUTHORITY\["EPSG",\s*"?(\d+)"?\]\s*\]\s*$/i);
  if (auth) {
    const p = fromEpsg(Number(auth[1]));
    if (p) return p;
  }
  if (!/^\s*PROJCS|^\s*PROJCRS/i.test(w)) return /^\s*GEOGCS|^\s*GEOGCRS|^\s*GEODCRS/i.test(w) ? GEOGRAPHIC : null;
  const name = (w.match(/^\s*PROJC(?:RS|S)\["([^"]+)"/i)?.[1] ?? "Projected").replace(/_/g, " ");
  const param = (...keys: string[]) => {
    for (const k of keys) {
      const m = w.match(new RegExp(`PARAMETER\\["${k}",\\s*(-?[\\d.eE+-]+)`, "i"));
      if (m) return Number(m[1]);
    }
    return undefined;
  };
  if (/Mercator_Auxiliary_Sphere|Popular_Visualisation|Pseudo.Mercator/i.test(w)) return WEB_MERCATOR;
  if (/Transverse_Mercator|Transverse Mercator/i.test(w)) {
    const zone = name.match(/(?:MGA|UTM)\s*zone\s*(\d+)/i);
    const lon0 = param("central_meridian", "Longitude of natural origin") ?? (zone ? Number(zone[1]) * 6 - 183 : undefined);
    if (lon0 == null) return null;
    return transverseMercator({
      name, lon0,
      k0: param("scale_factor", "Scale factor at natural origin") ?? 0.9996,
      x0: param("false_easting", "False easting") ?? 500000,
      y0: param("false_northing", "False northing") ?? 10000000,
      lat0: param("latitude_of_origin", "Latitude of natural origin") ?? 0,
    });
  }
  if (/Lambert_Conformal_Conic|Lambert Conic Conformal/i.test(w)) {
    const lat1 = param("standard_parallel_1", "Latitude of 1st standard parallel");
    const lat2 = param("standard_parallel_2", "Latitude of 2nd standard parallel");
    const lat0 = param("latitude_of_origin", "Latitude of false origin");
    const lon0 = param("central_meridian", "Longitude of false origin");
    if ([lat1, lat2, lat0, lon0].some((v) => v == null)) return null;
    return lambertConic2SP({ name, lat1: lat1!, lat2: lat2!, lat0: lat0!, lon0: lon0!, x0: param("false_easting", "Easting at false origin") ?? 0, y0: param("false_northing", "Northing at false origin") ?? 0 });
  }
  return null;
}

/** "EPSG:28354", "urn:ogc:def:crs:EPSG::7854", "http://www.opengis.net/def/crs/EPSG/0/3107" -> a projection. */
export function fromCrsName(name: string): Projection | null {
  if (/CRS84/i.test(name)) return GEOGRAPHIC;
  const m = name.match(/EPSG\D*(\d+)\s*$/i);
  return m ? fromEpsg(Number(m[1])) : null;
}
