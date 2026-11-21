#!/usr/bin/env python3
"""
make_basemap.py - build the offline basemap file for an EcoSyncField unit.

Takes one or more GeoJSON files (roads, reserve boundaries, management tracks),
clips them to a bounding box, simplifies them, strips everything the phone does
not need, and writes a small file for /web/basemap.geojson on the SD card.

NO DEPENDENCIES. Standard library only - nothing to pip install on the laptop,
nothing to break the next time Python updates.

Typical use, after exporting a clipped layer from ArcGIS:

    python make_basemap.py --in roads.geojson:road ^
                           --in reserve.geojson:bdy ^
                           --in tracks.geojson:track ^
                           --bbox 140.30,-34.30,140.90,-33.90 ^
                           --out basemap.geojson --gzip

Then copy basemap.geojson.gz to  <SD card>\\web\\  on the unit.

The unit serves <name>.gz under <name> with Content-Encoding: gzip, so the file
on the card is named basemap.geojson.gz and the page asks for basemap.geojson.
Leave it uncompressed if you prefer - the firmware serves either.
"""

import argparse
import gzip
import json
import math
import os
import sys

# ----------------------------------------------------------------------
# Geometry
#
# Everything is done in a local equirectangular approximation: longitude is
# scaled by cos(latitude) so that one unit east and one unit north are the same
# distance on the ground. Over a work area this is accurate to well under a
# metre, which is far better than the data and far better than the logger.
# ----------------------------------------------------------------------

R_EARTH = 6371008.8  # metres, mean radius


def _scale(lat_deg):
    """Metres per degree, (east, north), at this latitude."""
    north = math.radians(1.0) * R_EARTH
    east = north * math.cos(math.radians(lat_deg))
    return east, north


def _perp_distance_m(pt, a, b, mx, my):
    """Perpendicular distance in metres from pt to the segment a-b."""
    px, py = pt[0] * mx, pt[1] * my
    ax, ay = a[0] * mx, a[1] * my
    bx, by = b[0] * mx, b[1] * my
    dx, dy = bx - ax, by - ay
    if dx == 0.0 and dy == 0.0:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def simplify(points, tol_m, mx, my):
    """
    Douglas-Peucker, iterative rather than recursive.

    Recursion would be the obvious way to write this and would blow the stack on
    a long road, which is exactly the kind of input this gets.
    """
    n = len(points)
    if n < 3 or tol_m <= 0:
        return points

    keep = [False] * n
    keep[0] = keep[n - 1] = True
    stack = [(0, n - 1)]

    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        worst_d = -1.0
        worst_i = first
        a, b = points[first], points[last]
        for i in range(first + 1, last):
            d = _perp_distance_m(points[i], a, b, mx, my)
            if d > worst_d:
                worst_d, worst_i = d, i
        if worst_d > tol_m:
            keep[worst_i] = True
            stack.append((first, worst_i))
            stack.append((worst_i, last))

    return [p for i, p in enumerate(points) if keep[i]]


def bbox_of(coords, box=None):
    """Bounding box of an arbitrarily nested coordinate structure."""
    # Infinities, not ±180/±90: seeding with plausible lon/lat means a projected
    # layer (metres, values in the hundreds of thousands) never displaces the
    # minimum, and the error message then reports the seed instead of the data.
    if box is None:
        box = [math.inf, math.inf, -math.inf, -math.inf]
    if coords and isinstance(coords[0], (int, float)):
        x, y = coords[0], coords[1]
        box[0] = min(box[0], x)
        box[1] = min(box[1], y)
        box[2] = max(box[2], x)
        box[3] = max(box[3], y)
    else:
        for c in coords:
            bbox_of(c, box)
    return box


def boxes_overlap(a, b):
    return not (a[2] < b[0] or a[0] > b[2] or a[3] < b[1] or a[1] > b[3])


# ----------------------------------------------------------------------
# Processing
# ----------------------------------------------------------------------

def round_pt(p, dp):
    # 5 dp is about 1.1 m. Six is 0.1 m, which is meaningless against a logger
    # that reports 1.9 m average accuracy, and costs a byte per coordinate.
    return [round(p[0], dp), round(p[1], dp)]


def process_geometry(geom, bbox, tol_m, dp, stats):
    """Return a simplified geometry, or None if it falls outside the box."""
    if not geom:
        return None
    gtype = geom.get("type")
    coords = geom.get("coordinates")
    if coords is None:
        return None

    if not boxes_overlap(bbox_of(coords), bbox):
        return None

    mid_lat = (bbox[1] + bbox[3]) / 2.0
    mx, my = _scale(mid_lat)

    def do_line(line):
        stats["vin"] += len(line)
        out = simplify(line, tol_m, mx, my)
        stats["vout"] += len(out)
        return [round_pt(p, dp) for p in out]

    if gtype == "LineString":
        out = do_line(coords)
        return {"type": "LineString", "coordinates": out} if len(out) >= 2 else None

    if gtype == "MultiLineString":
        parts = [do_line(l) for l in coords]
        parts = [p for p in parts if len(p) >= 2]
        return {"type": "MultiLineString", "coordinates": parts} if parts else None

    if gtype == "Polygon":
        rings = [do_line(r) for r in coords]
        rings = [r for r in rings if len(r) >= 4]
        return {"type": "Polygon", "coordinates": rings} if rings else None

    if gtype == "MultiPolygon":
        polys = []
        for poly in coords:
            rings = [do_line(r) for r in poly]
            rings = [r for r in rings if len(r) >= 4]
            if rings:
                polys.append(rings)
        return {"type": "MultiPolygon", "coordinates": polys} if polys else None

    if gtype == "Point":
        stats["vin"] += 1
        stats["vout"] += 1
        return {"type": "Point", "coordinates": round_pt(coords, dp)}

    # GeometryCollection and anything exotic: skipped rather than guessed at.
    stats["skipped_types"].add(gtype)
    return None


def _box_inside(inner, outer):
    """Is one bounding box wholly within another?"""
    return (inner[0] >= outer[0] and inner[1] >= outer[1] and
            inner[2] <= outer[2] and inner[3] <= outer[3])


def esri_geometry_to_geojson(g):
    """Convert one Esri JSON geometry. Returns None if it is not one."""
    if not isinstance(g, dict):
        return None

    if "paths" in g:
        paths = [p for p in g["paths"] if len(p) >= 2]
        if not paths:
            return None
        if len(paths) == 1:
            return {"type": "LineString", "coordinates": paths[0]}
        return {"type": "MultiLineString", "coordinates": paths}

    if "rings" in g:
        # Esri's convention is that a clockwise ring is an outer boundary and a
        # counter-clockwise one is a hole. Containment is used here instead,
        # because a layer that does not follow the convention would otherwise
        # turn every hole into a separate filled polygon - and orientation is
        # not something an export is obliged to get right.
        polys = []
        boxes = []
        for ring in g["rings"]:
            if len(ring) < 4:
                continue
            rbox = bbox_of(ring)
            placed = False
            for i, obox in enumerate(boxes):
                if _box_inside(rbox, obox):
                    polys[i].append(ring)      # a hole in that polygon
                    placed = True
                    break
            if not placed:
                polys.append([ring])
                boxes.append(rbox)
        if not polys:
            return None
        if len(polys) == 1:
            return {"type": "Polygon", "coordinates": polys[0]}
        return {"type": "MultiPolygon", "coordinates": polys}

    if "x" in g and "y" in g:
        if g["x"] is None or g["y"] is None:
            return None
        return {"type": "Point", "coordinates": [g["x"], g["y"]]}

    return None


def merge_by_key(features):
    """
    Collapse every feature sharing a key into one multi-part geometry.

    Road networks are segmented for attribute reasons - a new record wherever
    the surface or the speed limit changes - so an LGA arrives as thousands of
    two-point fragments. That costs twice over: Leaflet builds a layer object
    per feature, which dominates render time on a phone, and every fragment
    repeats the same seventy-odd bytes of JSON scaffolding.

    Nothing is lost by merging, because every attribute has already been
    dropped and the map styles by key alone.
    """
    lines, polys, points, other = {}, {}, {}, []

    for f in features:
        key = f["properties"]["k"]
        g = f["geometry"]
        t = g["type"]
        if t == "LineString":
            lines.setdefault(key, []).append(g["coordinates"])
        elif t == "MultiLineString":
            lines.setdefault(key, []).extend(g["coordinates"])
        elif t == "Polygon":
            polys.setdefault(key, []).append(g["coordinates"])
        elif t == "MultiPolygon":
            polys.setdefault(key, []).extend(g["coordinates"])
        elif t == "Point":
            points.setdefault(key, []).append(g["coordinates"])
        else:
            other.append(f)

    out = []
    for key, parts in lines.items():
        out.append({"type": "Feature", "properties": {"k": key},
                    "geometry": {"type": "MultiLineString", "coordinates": parts}})
    for key, parts in polys.items():
        out.append({"type": "Feature", "properties": {"k": key},
                    "geometry": {"type": "MultiPolygon", "coordinates": parts}})
    for key, parts in points.items():
        out.append({"type": "Feature", "properties": {"k": key},
                    "geometry": {"type": "MultiPoint", "coordinates": parts}})
    return out + other


def load_source(path):
    """
    Reads GeoJSON or Esri JSON.

    ArcGIS's Features To JSON writes Esri JSON unless the GeoJSON box is ticked,
    and both land with a .json extension, so the difference is invisible until
    nothing works. Detecting it here costs thirty lines and saves a re-export.
    """
    with open(path, "r", encoding="utf-8-sig") as f:
        data = json.load(f)

    if data.get("type") == "FeatureCollection":
        return data.get("features", [])
    if data.get("type") == "Feature":
        return [data]

    # Esri JSON: a features list whose members carry "geometry" with paths,
    # rings or x/y, and "attributes" rather than "properties".
    if isinstance(data.get("features"), list):
        out = []
        for feat in data["features"]:
            g = esri_geometry_to_geojson(feat.get("geometry"))
            if g is not None:
                out.append({"type": "Feature", "properties": {}, "geometry": g})
        if out:
            print("  {}: read as Esri JSON ({:,} features)".format(
                os.path.basename(path), len(out)))
            return out

    # A bare geometry, in either dialect.
    g = esri_geometry_to_geojson(data)
    if g is not None:
        return [{"type": "Feature", "geometry": g, "properties": {}}]
    return [{"type": "Feature", "geometry": data, "properties": {}}]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inputs", action="append", required=True,
                    metavar="FILE[:KEY]",
                    help="input GeoJSON, optionally with the short key to tag it "
                         "with (road, track, bdy...). Repeatable.")
    ap.add_argument("--bbox", default="auto",
                    metavar="W,S,E,N",
                    help="clip extent in decimal degrees, e.g. 140.30,-34.30,140.90,-33.90. "
                         "Default 'auto' uses the extent of the input files, which is "
                         "what you want if you already clipped them in ArcGIS.")
    ap.add_argument("--pad-km", type=float, default=2.0,
                    help="expand the bbox by this many km on each side (default 2)")
    ap.add_argument("--tol", type=float, default=40.0,
                    help="simplification tolerance in metres (default 40). This is "
                         "orientation, not navigation - 30-50 is sensible.")
    ap.add_argument("--dp", type=int, default=5,
                    help="decimal places to keep (default 5, about 1.1 m)")
    ap.add_argument("--no-merge", action="store_true",
                    help="keep every source feature separate. The default merges "
                         "features sharing a key into one multi-part geometry, "
                         "which is much lighter for the phone to draw.")
    ap.add_argument("--out", default="basemap.geojson")
    ap.add_argument("--gzip", action="store_true",
                    help="also write <out>.gz, which is what goes on the card")
    args = ap.parse_args()

    # Resolve the input specs up front, so 'auto' can measure them.
    sources = []
    for spec in args.inputs:
        if ":" in spec and not os.path.exists(spec):
            path, key = spec.rsplit(":", 1)
        else:
            path, key = spec, "road"
        if not os.path.exists(path):
            sys.exit("No such file: " + path)
        sources.append((path, key, load_source(path)))

    # Measure the data extent whether or not we need it for --bbox, because it
    # is the only cheap way to catch the mistake that otherwise produces silent
    # nonsense: a layer exported in a PROJECTED system (MGA2020, eastings and
    # northings in metres) instead of geographic lon/lat. Degrees in, degrees
    # out - everything downstream, this script and Leaflet both, assumes it.
    box = [math.inf, math.inf, -math.inf, -math.inf]
    for _, _, feats in sources:
        for feat in feats:
            g = feat.get("geometry") or {}
            if g.get("coordinates") is not None:
                bbox_of(g["coordinates"], box)
    if box[0] > box[2]:
        sys.exit("Could not measure an extent - are the inputs empty?")

    if not (-180.0 <= box[0] <= 180.0 and -180.0 <= box[2] <= 180.0 and
            -90.0 <= box[1] <= 90.0 and -90.0 <= box[3] <= 90.0):
        sys.exit(
            "\nThese coordinates are not longitude/latitude.\n"
            "  measured extent: {:.1f},{:.1f} to {:.1f},{:.1f}\n\n"
            "That looks like a PROJECTED system - MGA2020 or similar, in metres.\n"
            "Re-export from ArcGIS in a GEOGRAPHIC system: GDA2020 (EPSG:7844)\n"
            "or WGS84 (EPSG:4326). Either is fine; they differ by about 40 cm,\n"
            "which is well inside the logger's accuracy.\n".format(*box))

    if args.bbox.strip().lower() == "auto":
        w, s, e, n = box
        print("\n  bbox auto-measured from the input files.")
    else:
        try:
            w, s, e, n = [float(v) for v in args.bbox.split(",")]
        except ValueError:
            sys.exit("--bbox must be four numbers: W,S,E,N (or 'auto')")
        if w > e or s > n:
            sys.exit("--bbox looks wrong: expected W,S,E,N with W<E and S<N")

    # Pad in degrees, using the latitude in the middle of the box.
    mid_lat = (s + n) / 2.0
    ex, ny = _scale(mid_lat)
    w -= (args.pad_km * 1000.0) / ex
    e += (args.pad_km * 1000.0) / ex
    s -= (args.pad_km * 1000.0) / ny
    n += (args.pad_km * 1000.0) / ny
    bbox = [w, s, e, n]

    stats = {"vin": 0, "vout": 0, "skipped_types": set()}
    features = []
    per_source = []

    for path, key, feats in sources:
        kept = 0
        for feat in feats:
            g = process_geometry(feat.get("geometry"), bbox, args.tol, args.dp, stats)
            if g is None:
                continue
            # Every attribute is dropped. The phone draws by key and nothing
            # else, and road names would roughly double the file for no gain on
            # a screen this size.
            features.append({"type": "Feature",
                             "properties": {"k": key},
                             "geometry": g})
            kept += 1
        per_source.append((os.path.basename(path), key, kept))

    if not features:
        sys.exit("Nothing fell inside the bbox - check the extent and the input CRS "
                 "(this expects WGS84 lon/lat, which is what GeoJSON should be).")

    before = len(features)
    if not args.no_merge:
        features = merge_by_key(features)

    out = {"type": "FeatureCollection",
           "features": features}

    # separators matter: the default ", " and ": " add a byte per element, which
    # over tens of thousands of coordinates is real.
    text = json.dumps(out, separators=(",", ":"))
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    raw = len(text.encode("utf-8"))

    gz_size = None
    if args.gzip:
        gz_path = args.out + ".gz"
        # mtime=0 so rebuilding identical input gives an identical file.
        with gzip.GzipFile(gz_path, "wb", compresslevel=9, mtime=0) as f:
            f.write(text.encode("utf-8"))
        gz_size = os.path.getsize(gz_path)

    print()
    print("  bbox (padded) : {:.5f},{:.5f},{:.5f},{:.5f}".format(*bbox))
    print("  tolerance     : {:g} m, {} dp".format(args.tol, args.dp))
    for name, key, kept in per_source:
        print("  {:<28} -> {:<6} {:>7} features".format(name, key, kept))
    if args.no_merge:
        print("  features kept : {:,}  (not merged)".format(len(features)))
    else:
        print("  features kept : {:,} -> merged into {:,} layer{}".format(
            before, len(features), "" if len(features) == 1 else "s"))
    print("  vertices      : {:,} in -> {:,} out  ({:.0f}% removed)".format(
        stats["vin"], stats["vout"],
        100.0 * (1 - stats["vout"] / stats["vin"]) if stats["vin"] else 0))
    print("  {:<14}: {:,} bytes".format(os.path.basename(args.out), raw))
    if gz_size:
        print("  {:<14}: {:,} bytes  <-- copy this to <SD>\\web\\".format(
            os.path.basename(args.out) + ".gz", gz_size))
    if stats["skipped_types"]:
        print("  skipped geometry types: " + ", ".join(sorted(stats["skipped_types"])))
    print()
    if (gz_size or raw) > 1_000_000:
        print("  NOTE: over 1 MB. That will be slow to fetch over the access point.")
        print("        Raise --tol or shrink the bbox.")
    print()


if __name__ == "__main__":
    main()
