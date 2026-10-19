#!/usr/bin/env python3
"""
MSA EcoSync - turn continuous track points into treated-area polygons.

The unit logs a point a second while the trigger is held, every point tagged
with the run_id of the squeeze it came from. This groups those points by run,
buffers them by the spray swath, dissolves the overlaps and writes the result
to the polygon layer as derived geometry.

    python track_to_polygons.py                      # today, dry run
    python track_to_polygons.py 2026-09-27           # a specific day
    python track_to_polygons.py 2026-09-27 --buffer 2.0
    python track_to_polygons.py 2026-09-27 --write   # actually post the polygons

A dry run always writes a GeoJSON into track_polygons\ beside this script so
you can drop it on a map and look at it before anything touches the layer.
Nothing is posted without --write.

WHY THE POLYGONS ARE BUILT HERE AND NOT ON THE UNIT
---------------------------------------------------
Because then they can be rebuilt. Change your mind about the swath, or find a
bug in this file, and you re-run it - the points are untouched and they are the
evidence. A ring built on the ESP32 and uploaded is the only copy there will
ever be, and we have already lost rings that way.

Credentials live in spray_stats_config.json beside this script - the same file
spray_stats.py uses. Never in here, never in git.

Needs `requests` and `shapely`:

    pip install requests shapely
"""

import argparse
import json
import math
import os
import random
import sys
from datetime import datetime, timedelta, timezone

try:
    from zoneinfo import ZoneInfo
except ImportError:
    print("This needs Python 3.9 or newer (for timezone handling).")
    sys.exit(1)

try:
    import requests
except ImportError:
    print("Missing `requests`.  Run:  pip install requests shapely")
    sys.exit(1)

try:
    from shapely.geometry import Point, mapping
    from shapely.ops import unary_union
    from shapely.geometry.polygon import orient
except ImportError:
    print("Missing `shapely`.  Run:  pip install requests shapely")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "spray_stats_config.json")

LOCAL_TZ = "Australia/Adelaide"

TRACK_URL = ("https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/"
             "services/MSA_Weed_Spray_Track/FeatureServer/0")
POLYS_URL = ("https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/"
             "services/MSA_Weed_Spray_Polygons/FeatureServer/0")

# Spray swath as a RADIUS in metres. This is the number that decides the area
# you report, so it is stored on every polygon it produces (buffer_m) rather
# than living only in this file. Override with --buffer.
DEFAULT_BUFFER_M = 1.5

# Circles are approximated by polygons; this is segments per quarter circle.
# The polygon is INSCRIBED, so it always understates the disc slightly: 8 gives
# a 32-gon at 0.62% low, 16 gives a 64-gon at 0.16%. 16 is used so the shapely
# area and the sampled cross-check below agree to well inside the tolerance,
# instead of always sitting half a percent apart for a reason that has nothing
# to do with whether the geometry is right.
QUAD_SEGS = 16


# ---------------------------------------------------------------------------
# Local metric projection.
#
# Buffering has to happen in metres, not degrees. Rather than pull in pyproj for
# one paddock, project to a local plane centred on the data. Over a few km the
# error is far below the receiver's own accuracy, and the formulas below are the
# standard series for metres per degree at a given latitude - not the flat
# 111320 approximation, because this produces a compliance area figure.
# ---------------------------------------------------------------------------
def scale_at(lat_deg):
    phi = math.radians(lat_deg)
    m_lat = (111132.92
             - 559.82 * math.cos(2 * phi)
             + 1.175 * math.cos(4 * phi)
             - 0.0023 * math.cos(6 * phi))
    m_lon = (111412.84 * math.cos(phi)
             - 93.5 * math.cos(3 * phi)
             + 0.118 * math.cos(5 * phi))
    return m_lat, m_lon


def load_config():
    if not os.path.exists(CONFIG_PATH):
        print("No config found at:\n  %s\n" % CONFIG_PATH)
        print("Copy spray_stats_config.example.json to spray_stats_config.json "
              "and fill in your credentials.")
        sys.exit(1)
    with open(CONFIG_PATH) as f:
        return json.load(f)


def get_token(cfg):
    if cfg.get("api_key"):
        return cfg["api_key"]
    r = requests.post(
        "https://www.arcgis.com/sharing/rest/generateToken",
        data={"username": cfg["username"], "password": cfg["password"],
              "referer": "https://www.arcgis.com", "expiration": 60, "f": "json"},
        timeout=30)
    r.raise_for_status()
    body = r.json()
    if "token" not in body:
        print("Could not sign in: %s"
              % body.get("error", {}).get("message", "unknown error"))
        sys.exit(1)
    return body["token"]


def day_window_ms(day_str):
    tz = ZoneInfo(LOCAL_TZ)
    day = datetime.strptime(day_str, "%Y-%m-%d").replace(tzinfo=tz)
    start = day.replace(hour=0, minute=0, second=0, microsecond=0)
    return int(start.timestamp() * 1000), int((start + timedelta(days=1)).timestamp() * 1000)


# What we would LIKE from the track layer. Nothing here is mandatory except a
# geometry - a layer missing weed_species still produces polygons, just without
# that attribute on them.
WANTED_FIELDS = ["run_id", "capture_mode", "weed_species", "control_type",
                 "gps_accuracy_m", "treatment_time", "Job_Number", "job_id"]


def layer_fields(url, token):
    """Ask the layer what it actually has, rather than assuming.

    ArcGIS answers a query naming one unknown field with "Invalid query
    parameters" and no hint as to which, so guessing costs more than asking.
    """
    r = requests.post(url, data={"f": "json", "token": token}, timeout=60)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError(body["error"].get("message", body["error"]))
    return {f["name"]: f.get("type", "") for f in body.get("fields", [])}


def time_clauses(t0, t1, ftype):
    """Both accepted spellings of a date range, most likely first.

    A hosted service normally accepts epoch milliseconds directly against a
    date field. Some do not, and want a TIMESTAMP literal in UTC. Rather than
    decide which from the field type alone, try one and fall back.
    """
    epoch = "treatment_time >= %d AND treatment_time < %d" % (t0, t1)
    fmt = "%Y-%m-%d %H:%M:%S"
    a = datetime.fromtimestamp(t0 / 1000.0, timezone.utc).strftime(fmt)
    b = datetime.fromtimestamp(t1 / 1000.0, timezone.utc).strftime(fmt)
    stamp = ("treatment_time >= TIMESTAMP '%s' AND treatment_time < TIMESTAMP '%s'"
             % (a, b))
    return [epoch, stamp] if ftype != "esriFieldTypeString" else [epoch]


def fetch_track(token, t0, t1):
    """Every track point in the window, paged, using only fields that exist."""
    fields = layer_fields(TRACK_URL, token)
    present = [f for f in WANTED_FIELDS if f in fields]
    missing = [f for f in WANTED_FIELDS if f not in fields]

    print("Layer fields   : %s" % ", ".join(sorted(fields.keys())))
    if missing:
        print("Not on layer   : %s" % ", ".join(missing))
        if "run_id" in missing:
            print("                 ** without run_id every point becomes its own run;")
            print("                    the spatial merge still works but the grouping")
            print("                    is inferred rather than recorded **")

    oid = "OBJECTID" if "OBJECTID" in fields else None
    out_fields = ",".join(present + ([oid] if oid and oid not in present else []))
    if not out_fields:
        out_fields = "*"

    if "treatment_time" in fields:
        clauses = time_clauses(t0, t1, fields["treatment_time"])
    else:
        print("                 ** no treatment_time - taking the WHOLE layer, not one day **")
        clauses = ["1=1"]

    last_err = None
    for where in clauses:
        try:
            return _paged_query(token, where, out_fields)
        except RuntimeError as e:
            last_err = e
            if len(clauses) > 1:
                print("                 (that date syntax was rejected, trying the other)")
    raise last_err


def describe_layer(token):
    """What the track layer actually holds, for when a day comes back empty.

    An empty result is ambiguous - wrong day, or nothing synced? Asking the
    layer for its record count and date range answers it immediately instead
    of costing a round of guessing.
    """
    try:
        r = requests.post(TRACK_URL + "/query",
                          data={"f": "json", "token": token, "where": "1=1",
                                "returnCountOnly": "true"}, timeout=60)
        total = r.json().get("count")

        stats = json.dumps([
            {"statisticType": "min", "onStatisticField": "treatment_time",
             "outStatisticFieldName": "t_min"},
            {"statisticType": "max", "onStatisticField": "treatment_time",
             "outStatisticFieldName": "t_max"},
        ])
        r = requests.post(TRACK_URL + "/query",
                          data={"f": "json", "token": token, "where": "1=1",
                                "outStatistics": stats}, timeout=60)
        body = r.json()
        a = body["features"][0]["attributes"]
        tz = ZoneInfo(LOCAL_TZ)
        lo = datetime.fromtimestamp(a["t_min"] / 1000.0, tz)
        hi = datetime.fromtimestamp(a["t_max"] / 1000.0, tz)
        return total, lo, hi
    except Exception:
        return None, None, None


def _paged_query(token, where, out_fields):
    out = []
    offset = 0
    while True:
        p = {
            "f": "json", "token": token,
            "where": where,
            "outFields": out_fields,
            "returnGeometry": "true",
            "outSR": 4326,
            "resultOffset": offset,
            "resultRecordCount": 2000,
        }
        r = requests.post(TRACK_URL + "/query", data=p, timeout=90)
        r.raise_for_status()
        body = r.json()
        if "error" in body:
            raise RuntimeError(body["error"].get("message", body["error"]))
        feats = body.get("features", [])
        out.extend(feats)
        if not body.get("exceededTransferLimit") or not feats:
            break
        offset += len(feats)
    return out


RUN_KEY_SEP = "\x1f"        # unit separator; cannot appear in a run_id

# job_id on the layer is a text field of unknown length, and a rejected insert
# on the last step of the day is a bad trade for perfect provenance.
JOB_ID_MAX = 50


def join_units(units):
    """The units that contributed to one polygon, as one field value."""
    if not units:
        return None
    s = ", ".join(units)
    return s if len(s) <= JOB_ID_MAX else (s[:JOB_ID_MAX - 3] + "...")


def group_runs(features):
    """Bucket points by UNIT and run_id.

    A missing run_id is not a reason to drop a record - it just means the point
    came from firmware older than the run tagging, or the field was added after
    it synced. Treat it as a run of one rather than losing the spray event.

    THE UNIT HAS TO BE PART OF THE KEY.
    --------------------------------------------------------------------------
    A run_id is the epoch SECOND the trigger went down (firmware:
    currentRunId = live_epoch). That is unique for one unit and not remotely
    unique across two. Two operators squeezing in the same second produce the
    same run_id, and bucketing on run_id alone silently merges their points
    into one run - which then buffers and dissolves into a single polygon
    spanning whatever distance separates them.

    It is not a rare case either. A dodder day is on the order of 170 runs; two
    units over an eight-hour day gives roughly 170 * 170 / 28800, about one
    collision per day, every day, with no error and a plausible-looking shape.

    job_id carries the unit (MSA_EcoSync_001, _002, ...). Keying on it here
    fixes historical data too, which a firmware change could not.
    """
    runs = {}
    orphans = 0
    for f in features:
        a = f.get("attributes", {}) or {}
        g = f.get("geometry") or {}
        if g.get("x") is None or g.get("y") is None:
            continue
        rid = a.get("run_id")
        if rid in (None, "", "0"):
            orphans += 1
            rid = "orphan-%d" % (a.get("OBJECTID") or orphans)
        unit = a.get("job_id") or ""
        runs.setdefault(str(unit) + RUN_KEY_SEP + str(rid), []).append((a, g))
    return runs, orphans


def build_polygons(runs, buffer_m, merge=True):
    """Buffer every point, dissolve, and return (shapely_geom, attrs) pairs.

    Everything happens in a local metric plane. Runs are buffered and unioned
    individually first, then - unless --no-merge - overlapping runs are merged
    so the reported area is not double-counted. A patch worked in five separate
    squeezes is one treated area, not five.
    """
    all_pts = [g for pts in runs.values() for (_a, g) in pts]
    if not all_pts:
        return [], []
    lat0 = sum(g["y"] for g in all_pts) / len(all_pts)
    lon0 = sum(g["x"] for g in all_pts) / len(all_pts)
    m_lat, m_lon = scale_at(lat0)

    def to_m(x, y):
        return ((x - lon0) * m_lon, (y - lat0) * m_lat)

    def to_deg(x, y):
        return (lon0 + x / m_lon, lat0 + y / m_lat)

    pieces = []
    for key, pts in runs.items():
        # The key carries the unit; the run id that goes onto the polygon is
        # the bare one, so the value still matches the Track layer's run_id.
        rid = key.split(RUN_KEY_SEP, 1)[-1]

        # Positional, not keyword: shapely 1.8 calls this argument `resolution`
        # and 2.x calls it `quad_segs`. Positional works on both.
        circles = [Point(to_m(g["x"], g["y"])).buffer(buffer_m, QUAD_SEGS)
                   for (_a, g) in pts]
        geom = unary_union(circles)

        accs = [a.get("gps_accuracy_m") for (a, _g) in pts
                if a.get("gps_accuracy_m") is not None]
        times = [a.get("treatment_time") for (a, _g) in pts
                 if a.get("treatment_time") is not None]
        first = pts[0][0]
        pieces.append({
            "geom": geom,
            "run_ids": [rid],
            "n": len(pts),
            # Worst accuracy anywhere in the run, matching how a walked polygon
            # stores its value - a ceiling for the whole shape, not an average.
            "worst_acc": max(accs) if accs else None,
            "t0": min(times) if times else None,
            "weed": first.get("weed_species"),
            "control": first.get("control_type"),
            "job": first.get("Job_Number"),
            "job_id": first.get("job_id"),
        })

    if merge:
        pieces = merge_overlapping(pieces)

    out = []
    for p in pieces:
        geom = p["geom"]
        parts = list(geom.geoms) if geom.geom_type == "MultiPolygon" else [geom]
        for part in parts:
            # ArcGIS reads a counter-clockwise outer ring as a hole, so force
            # clockwise. orient(sign=-1.0) also flips interior rings correctly.
            part = orient(part, sign=-1.0)
            rings = [[list(to_deg(x, y)) for (x, y) in part.exterior.coords]]
            for interior in part.interiors:
                rings.append([list(to_deg(x, y)) for (x, y) in interior.coords])
            out.append({
                "rings": rings,
                "area_m2": part.area,
                "attrs": p,
            })
    return out, [to_m(g["x"], g["y"]) for g in all_pts]


def mc_area(points_m, radius, per_point=None, seed=12345):
    """An INDEPENDENT estimate of the same area, in pure Python.

    This exists because the polygon geometry goes through shapely and the
    number it produces ends up on a compliance record. This shares no code with
    the buffer-and-union path, so agreement between the two is real evidence.

    The estimator uses the identity

        area(union of discs) = sum over i of integral over disc_i of 1/m(x)

    where m(x) counts how many discs cover x. Sampling inside each disc and
    averaging 1/m gives that integral, so EVERY sample lands inside the union
    and contributes.

    That matters. The first version of this sampled the overall bounding box
    and asked whether each sample fell in the union - which is fine for one
    tight cluster and useless for a morning's work spread across a paddock,
    where the hit rate collapses to a fraction of a percent and the estimate
    turns to noise. It reported a 6.3% disagreement on real data that was
    entirely its own sampling error. This version's accuracy does not depend on
    how scattered the work is: it holds to about 0.1% either way.
    """
    if not points_m:
        return 0.0

    # Aim for a roughly constant total sample count rather than a constant
    # count per point: a day with three points needs far more samples each to
    # settle than a day with three thousand, where averaging across points does
    # most of the work.
    if per_point is None:
        per_point = max(200, min(20000, 240000 // len(points_m)))

    cell = radius
    grid = {}
    for (x, y) in points_m:
        grid.setdefault((int(x // cell), int(y // cell)), []).append((x, y))

    r2 = radius * radius
    disc = math.pi * r2
    rnd = random.Random(seed)
    total = 0.0

    for (px0, py0) in points_m:
        acc = 0.0
        for _ in range(per_point):
            # Uniform inside the disc: sqrt() on the radius, or samples bunch
            # towards the centre and every overlap is mis-weighted.
            theta = rnd.random() * 2.0 * math.pi
            rr = radius * math.sqrt(rnd.random())
            px = px0 + rr * math.cos(theta)
            py = py0 + rr * math.sin(theta)

            m = 0
            cx, cy = int(px // cell), int(py // cell)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for (qx, qy) in grid.get((cx + dx, cy + dy), ()):
                        if (px - qx) ** 2 + (py - qy) ** 2 <= r2:
                            m += 1
            if m:
                acc += 1.0 / m
        total += disc * acc / per_point

    return total


def merge_overlapping(pieces):
    """Union runs whose buffers touch, so shared ground is counted once.

    Simple union-find over pairwise intersection. There are tens of runs in a
    day, not thousands, so the quadratic pass is not worth optimising.
    """
    parent = list(range(len(pieces)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        ri, rj = find(i), find(j)
        if ri != rj:
            parent[rj] = ri

    for i in range(len(pieces)):
        for j in range(i + 1, len(pieces)):
            if pieces[i]["geom"].intersects(pieces[j]["geom"]):
                union(i, j)

    groups = {}
    for i in range(len(pieces)):
        groups.setdefault(find(i), []).append(pieces[i])

    merged = []
    for members in groups.values():
        if len(members) == 1:
            merged.append(members[0])
            continue
        accs = [m["worst_acc"] for m in members if m["worst_acc"] is not None]
        times = [m["t0"] for m in members if m["t0"] is not None]
        ids = sorted(set(rid for m in members for rid in m["run_ids"]))

        # Two units CAN legitimately overlap - same ground, same day, two
        # operators - and merging them is right, because the area was sprayed
        # once as far as a hectare figure is concerned. What is not right is
        # quietly stamping one unit's id on the result, so say both.
        units = sorted({m["job_id"] for m in members if m["job_id"]})
        if len(units) > 1:
            print("Note           : merged polygon spans %s - overlapping ground, "
                  "counted once" % ", ".join(units))

        merged.append({
            "geom": unary_union([m["geom"] for m in members]),
            "run_ids": ids,
            "n": sum(m["n"] for m in members),
            "worst_acc": max(accs) if accs else None,
            "t0": min(times) if times else None,
            "weed": members[0]["weed"],
            "control": members[0]["control"],
            "job": members[0]["job"],
            "job_id": join_units(units) or members[0]["job_id"],
        })
    return merged


def to_esri_features(polys, buffer_m):
    feats = []
    for p in polys:
        a = p["attrs"]
        attrs = {
            "weed_species": a["weed"],
            "geom_source": "derived",
            "buffer_m": buffer_m,
            # The EARLIEST run in a merged polygon. The full list is in the
            # GeoJSON if you ever need to trace every contributing squeeze.
            "run_id": a["run_ids"][0],
        }
        if a["t0"] is not None:
            attrs["treatment_time"] = a["t0"]
        if a["control"]:
            attrs["control_type"] = a["control"]
        if a["job"]:
            attrs["Job_Number"] = a["job"]
        if a["job_id"]:
            attrs["job_id"] = a["job_id"]
        if a["worst_acc"] is not None:
            attrs["gps_accuracy_m"] = round(float(a["worst_acc"]), 2)
        feats.append({
            "geometry": {"rings": p["rings"],
                         "spatialReference": {"wkid": 4326}},
            "attributes": attrs,
        })
    return feats


def write_geojson(polys, path, buffer_m):
    """RFC 7946, which is NOT the same winding ArcGIS wants.

    The rings built above are clockwise because that is what a hosted feature
    service reads as an outer ring. GeoJSON specifies the right-hand rule -
    exterior counter-clockwise, holes clockwise - and a strict reader rejects
    the file outright rather than guessing. Reversing every ring flips both at
    once.

    Nulls are coerced to empty strings for the same reason: a text property
    that is null in every feature gives a reader nothing to infer a type from,
    and some of them fail the whole file over it.
    """
    def s(v):
        return "" if v is None else str(v)

    fc = {"type": "FeatureCollection", "features": []}
    for p in polys:
        a = p["attrs"]
        rings = [list(reversed(r)) for r in p["rings"]]
        fc["features"].append({
            "type": "Feature",
            "geometry": {"type": "Polygon", "coordinates": rings},
            "properties": {
                "weed_species": s(a["weed"]),
                "control_type": s(a["control"]),
                "Job_Number": s(a["job"]),
                "run_ids": ",".join(a["run_ids"]),
                "run_count": len(a["run_ids"]),
                "point_count": a["n"],
                "gps_accuracy_m": (float(a["worst_acc"])
                                   if a["worst_acc"] is not None else 0.0),
                "buffer_m": float(buffer_m),
                "area_m2": round(p["area_m2"], 2),
            },
        })
    with open(path, "w") as f:
        json.dump(fc, f)
    return path


def post_features(token, feats):
    r = requests.post(POLYS_URL + "/addFeatures",
                      data={"f": "json", "token": token,
                            "features": json.dumps(feats)},
                      timeout=120)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError(body["error"].get("message", body["error"]))
    ok = sum(1 for x in body.get("addResults", []) if x.get("success"))
    bad = [x for x in body.get("addResults", []) if not x.get("success")]
    return ok, bad


def main():
    ap = argparse.ArgumentParser(description="Build treated-area polygons from EcoSync track points.")
    ap.add_argument("day", nargs="?", help="YYYY-MM-DD (default: today, local time)")
    ap.add_argument("--buffer", type=float, default=DEFAULT_BUFFER_M,
                    help="spray swath RADIUS in metres (default %.1f)" % DEFAULT_BUFFER_M)
    ap.add_argument("--no-merge", action="store_true",
                    help="one polygon per run, overlaps left in place (areas will double-count)")
    ap.add_argument("--write", action="store_true",
                    help="post the polygons to the polygon layer")
    ap.add_argument("--fields", action="store_true",
                    help="print both layers' field names and exit")
    args = ap.parse_args()

    if args.fields:
        cfg = load_config()
        token = get_token(cfg)
        for label, url in (("TRACK", TRACK_URL), ("POLYGONS", POLYS_URL)):
            print("%s:" % label)
            try:
                for name, ftype in sorted(layer_fields(url, token).items()):
                    print("   %-20s %s" % (name, ftype.replace("esriFieldType", "")))
            except Exception as e:
                print("   could not read: %s" % e)
            print()
        return

    day = args.day or datetime.now(ZoneInfo(LOCAL_TZ)).strftime("%Y-%m-%d")
    t0, t1 = day_window_ms(day)

    cfg = load_config()
    token = get_token(cfg)

    print("Day            : %s (%s)" % (day, LOCAL_TZ))
    print("Buffer radius  : %.2f m" % args.buffer)
    print()

    feats = fetch_track(token, t0, t1)
    print("Track points   : %d" % len(feats))
    if not feats:
        total, lo, hi = describe_layer(token)
        if total:
            print()
            print("The layer holds %d records, from %s to %s (local time)."
                  % (total, lo.strftime("%Y-%m-%d %H:%M"), hi.strftime("%Y-%m-%d %H:%M")))
            if lo.date() == hi.date():
                print("Try:  track_to_polygons.bat %s" % lo.strftime("%Y-%m-%d"))
            else:
                print("Pick a date in that range.")
        elif total == 0:
            print("\nThe layer is empty - nothing has synced into it yet.")
        else:
            print("\nNothing for that day.")
        return

    runs, orphans = group_runs(feats)
    print("Runs           : %d" % len(runs))
    if orphans:
        print("                 (%d points had no run_id - treated as runs of one)" % orphans)

    lens = sorted(len(v) for v in runs.values())
    print("Points per run : min %d, median %d, max %d"
          % (lens[0], lens[len(lens) // 2], lens[-1]))
    if lens[len(lens) // 2] <= 1:
        print("                 ** median of 1 means runs are ending after a single")
        print("                    point - the release hold is not doing its job **")

    polys, pts_m = build_polygons(runs, args.buffer, merge=not args.no_merge)
    total = sum(p["area_m2"] for p in polys)
    print()
    print("Polygons       : %d" % len(polys))
    print("Treated area   : %.0f m2  (%.3f ha)" % (total, total / 10000.0))
    if polys:
        areas = sorted(p["area_m2"] for p in polys)
        print("Per polygon    : min %.1f, median %.1f, max %.1f m2"
              % (areas[0], areas[len(areas) // 2], areas[-1]))

    # Cross-check against a method that shares no code with the one above.
    if not args.no_merge:
        check = mc_area(pts_m, args.buffer)
        drift = abs(check - total) / total * 100.0 if total else 0.0
        print("Cross-check    : %.0f m2 by sampling (%.2f%% apart)" % (check, drift))
        if drift > 1.5:
            print("                 ** these should agree to a few tenths of a percent.")
            print("                    Do not post until the difference is understood. **")

    # One GeoJSON per day, kept rather than overwritten - they are the record of
    # what was posted and with what swath - so they get their own folder instead
    # of silting up the tools directory.
    out_dir = os.path.join(HERE, "track_polygons")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "track_polygons_%s.geojson" % day)
    write_geojson(polys, out, args.buffer)
    print()
    print("GeoJSON written: %s" % out)

    if not args.write:
        print()
        print("Dry run - nothing posted. Look at the GeoJSON, then re-run with --write.")
        return

    esri = to_esri_features(polys, args.buffer)
    ok, bad = post_features(token, esri)
    print()
    print("Posted         : %d of %d" % (ok, len(esri)))
    for b in bad[:5]:
        print("  failed: %s" % b.get("error", {}).get("description", b))
    if bad:
        print("  NOTE: re-running after a partial failure will duplicate the ones")
        print("        that succeeded. Delete this day's derived polygons first:")
        print("        geom_source = 'derived' AND treatment_time BETWEEN %d AND %d" % (t0, t1))


if __name__ == "__main__":
    main()
