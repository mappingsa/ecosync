#!/usr/bin/env python3
"""
MSA EcoSync - spray report accuracy statistics.

Queries the EcoSync point and polygon layers for one day's work, computes the
positional-quality figures the spray report prints, and optionally writes them
straight into that day's Survey123 record so the report fills itself in.

    python spray_stats.py                      # today, print the figures
    python spray_stats.py 2026-09-19           # a specific day
    python spray_stats.py 2026-09-19 --showextent   # + what the web map SHOULD show
    python spray_stats.py 2026-09-19 --setextent    # + actually reposition the web map
    python spray_stats.py 2026-09-19 --write        # + fill the Survey123 record

Typical end of day:

    spray_stats.bat 2026-09-19 --setextent --write

then open Survey123 and generate the report - the map is already framed on the
day's data and the accuracy figures are already in the record.

Add --focus to frame on the day's real work when a few pulls on the drive in
would otherwise stretch the map across twenty kilometres.

Credentials live in spray_stats_config.json beside this script - never in here,
and never in git. Copy spray_stats_config.example.json and fill it in.

Only needs `requests`. No ArcGIS Python API, no conda environment.
"""

import json
import math
import os
import re
import sys
from datetime import datetime, timedelta, timezone

try:
    from zoneinfo import ZoneInfo          # Python 3.9+
except ImportError:
    print("This needs Python 3.9 or newer (for timezone handling).")
    sys.exit(1)

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "spray_stats_config.json")

# The unit records treatment_time as epoch MILLISECONDS in UTC. A working day is
# a LOCAL day, and South Australia shifts between ACST and ACDT, so the window
# has to be built in the local zone and converted - not assumed to be UTC.
LOCAL_TZ = "Australia/Adelaide"

POINTS_URL = ("https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/"
              "services/MSA_Weed_Spray_Points/FeatureServer/0")
POLYS_URL  = ("https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/"
              "services/MSA_Weed_Spray_Polygons/FeatureServer/0")
SURVEY_URL = ("https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/"
              "services/service_d66efa637ea6410badeae3eb893e3740/FeatureServer/0")

# The web map the report template draws. With ${$map| map:"..."} the report uses
# this web map's SAVED extent - which is why repositioning and saving it by hand
# was part of every report. --setextent writes the day's bounding box into it.
WEBMAP_ITEM_ID = "d7f8b5123e1b48488faf8b082f4e2393"
PORTAL = "https://www.arcgis.com"

ACCURACY_THRESHOLD_M = 3.0      # the "within N m" figure on the report
EXTENT_MARGIN = 0.12            # padding around the data, as a fraction of its size
MIN_SPAN_DEG = 0.0025           # ~250 m, so a single point does not zoom to infinity

# Shape of the map image the report template asks for: ${$map| ... | size:600:400}.
# The extent is grown to this shape so the renderer has nothing to crop. Change
# it here if the template's size changes, or pass --mapsize 800x500.
MAP_ASPECT = 600.0 / 400.0


def load_config():
    if not os.path.exists(CONFIG_PATH):
        print("No config found at:\n  %s\n" % CONFIG_PATH)
        print("Copy spray_stats_config.example.json to spray_stats_config.json "
              "and fill in your credentials.")
        sys.exit(1)
    with open(CONFIG_PATH) as f:
        raw = f.read()
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        # A raw traceback for a missing comma is no help to anyone. Show where
        # it is - but print only the KEY on each line, never the value, because
        # this file holds a password and the whole point of an error message is
        # that it is safe to read out or paste to someone.
        print("\nspray_stats_config.json is not valid JSON.\n")
        print("  %s, line %d column %d\n" % (e.msg, e.lineno, e.colno))
        lines = raw.splitlines()
        for n in range(max(0, e.lineno - 3), min(len(lines), e.lineno + 1)):
            text = lines[n]
            m = re.match(r'^(\s*"[^"]*"\s*:\s*)(.*)$', text)
            if m:
                tail = m.group(2).rstrip()
                # Keep structural punctuation, hide the value itself.
                suffix = "," if tail.endswith(",") else ("{" if tail.endswith("{") else "")
                text = m.group(1) + ("{" if suffix == "{" else "...") + \
                       ("," if suffix == "," else "")
            print("  %s %3d | %s" % (">>" if n == e.lineno - 1 else "  ", n + 1, text))
        print()
        if "Expecting ',' delimiter" in e.msg:
            print("  That almost always means the line ABOVE the marked one is missing")
            print("  a trailing comma. Every entry except the last needs one.")
        elif "Expecting property name" in e.msg:
            print("  Usually a trailing comma after the LAST entry - the last one")
            print("  must not have one.")
        print("  Values are hidden above; only the structure is shown.\n")
        sys.exit(1)


def get_token(cfg):
    """An api_key is used as-is; otherwise swap username/password for a token."""
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
        # Never echo the response wholesale - it can contain credentials.
        print("Could not sign in: %s" % body.get("error", {}).get("message", "unknown error"))
        sys.exit(1)
    return body["token"]


def day_window_ms(day_str):
    """Local midnight-to-midnight for the given date, as epoch milliseconds."""
    tz = ZoneInfo(LOCAL_TZ)
    day = datetime.strptime(day_str, "%Y-%m-%d").replace(tzinfo=tz)
    start = day.replace(hour=0, minute=0, second=0, microsecond=0)
    end = start + timedelta(days=1)
    return int(start.timestamp() * 1000), int(end.timestamp() * 1000)


def query(url, token, params):
    p = {"f": "json", "token": token}
    p.update(params)
    r = requests.post(url + "/query", data=p, timeout=60)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError("%s: %s" % (url.rsplit("/services/", 1)[-1],
                                       body["error"].get("message", body["error"])))
    return body


# Which spelling of a date range each service accepts, decided once per layer.
#
# A hosted service normally takes epoch milliseconds straight against a date
# field. Some reject it and want a TIMESTAMP literal in UTC instead - the track
# layer does exactly that. Probing beats assuming, and the answer differs
# between services in the same org, so it is cached per URL rather than once.
_DATE_STYLE = {}


def date_where(url, token, field, t0, t1):
    def epoch():
        return "%s >= %d AND %s < %d" % (field, t0, field, t1)

    def stamp():
        fmt = "%Y-%m-%d %H:%M:%S"
        a = datetime.fromtimestamp(t0 / 1000.0, timezone.utc).strftime(fmt)
        b = datetime.fromtimestamp(t1 / 1000.0, timezone.utc).strftime(fmt)
        return "%s >= TIMESTAMP '%s' AND %s < TIMESTAMP '%s'" % (field, a, field, b)

    key = (url, field)
    if key not in _DATE_STYLE:
        try:
            query(url, token, {"where": epoch(), "returnCountOnly": "true"})
            _DATE_STYLE[key] = "epoch"
        except RuntimeError:
            _DATE_STYLE[key] = "timestamp"
    return epoch() if _DATE_STYLE[key] == "epoch" else stamp()


def field_named(url, token, name):
    """The layer's actual spelling of a field, or None if it has no such field.

    Job_Number on the feature layers, job_number on the Survey123 layer - the
    same idea under two spellings, and a where clause needs the real one.
    """
    for f in layer_meta(url, token).get("fields", []):
        if f.get("name", "").lower() == name.lower():
            return f["name"]
    return None


def sql_str(v):
    return "'" + str(v).replace("'", "''") + "'"


def job_clause(url, token, job):
    """AND-able job filter, or empty when the layer cannot be filtered by job."""
    if not job:
        return ""
    f = field_named(url, token, "Job_Number")
    return "" if not f else " AND %s = %s" % (f, sql_str(job))


def layer_stats(url, token, t0, t1, job=None):
    """count, sum and worst of gps_accuracy_m, plus how many are within the threshold.

    Records with a NULL accuracy are counted as captured but excluded from the
    accuracy maths - older firmware did not write the field, and averaging over
    a missing value would quietly understate the figure.
    """
    where_day = date_where(url, token, "treatment_time", t0, t1) + job_clause(url, token, job)

    total = query(url, token, {"where": where_day, "returnCountOnly": "true"})["count"]

    stats = query(url, token, {
        "where": where_day + " AND gps_accuracy_m IS NOT NULL",
        "outStatistics": json.dumps([
            {"statisticType": "count", "onStatisticField": "gps_accuracy_m",
             "outStatisticFieldName": "n"},
            {"statisticType": "sum", "onStatisticField": "gps_accuracy_m",
             "outStatisticFieldName": "total"},
            {"statisticType": "max", "onStatisticField": "gps_accuracy_m",
             "outStatisticFieldName": "worst"},
        ])})
    row = stats.get("features", [{}])[0].get("attributes", {})

    within = query(url, token, {
        "where": where_day + " AND gps_accuracy_m <= %s" % ACCURACY_THRESHOLD_M,
        "returnCountOnly": "true"})["count"]

    return dict(total=total,
                n=row.get("n") or 0,
                sum=row.get("total") or 0.0,
                worst=row.get("worst"),
                within=within)


_LAYER_META = {}


def layer_meta(url, token):
    """The layer's own description - cached. Tells us what the fields are
    really called rather than assuming.

    The object-id field is NOT always OBJECTID. A service will happily accept
    'OBJECTID' in outFields case-insensitively and then hand the value back
    under whatever name it actually uses, which is how this fell over with a
    KeyError on a query that had plainly succeeded.
    """
    if url not in _LAYER_META:
        r = requests.post(url, data={"f": "json", "token": token}, timeout=60)
        r.raise_for_status()
        body = r.json()
        if "error" in body:
            raise RuntimeError(body["error"].get("message", body["error"]))
        _LAYER_META[url] = body
    return _LAYER_META[url]


def oid_field(url, token):
    return layer_meta(url, token).get("objectIdField") or "OBJECTID"


def attr(attrs, name):
    """Case-insensitive attribute lookup, because services are inconsistent."""
    if name in attrs:
        return attrs[name]
    low = name.lower()
    for k, v in attrs.items():
        if k.lower() == low:
            return v
    return None


def find_survey_record(token, day_str, job=None):
    """The Survey123 record for that spray_date and job. Returns (object id, job)."""
    t0, t1 = day_window_ms(day_str)
    oidf = oid_field(SURVEY_URL, token)
    fields = {f["name"] for f in layer_meta(SURVEY_URL, token).get("fields", [])}
    want = [oidf] + [f for f in ("job_number",) if f in fields]

    where = date_where(SURVEY_URL, token, "spray_date", t0, t1)
    jf = field_named(SURVEY_URL, token, "job_number")
    if job and jf:
        where += " AND %s = %s" % (jf, sql_str(job))

    body = query(SURVEY_URL, token, {
        "where": where,
        "outFields": ",".join(want), "returnGeometry": "false"})
    feats = body.get("features", [])
    if not feats:
        return None, None
    if len(feats) > 1:
        print("  NOTE: %d survey records share that date - updating the first "
              "(%s %s)." % (len(feats), oidf, attr(feats[0]["attributes"], oidf)))
    a = feats[0]["attributes"]
    return attr(a, oidf), attr(a, "job_number")


def write_back(token, oid, values):
    oidf = oid_field(SURVEY_URL, token)
    fields = {f["name"].lower() for f in layer_meta(SURVEY_URL, token).get("fields", [])}

    # Only send fields the layer actually has. applyEdits will happily report
    # success while silently dropping an attribute it does not recognise, so a
    # missing acc_* field would look like it worked and print nothing.
    missing = [k for k in values if k.lower() not in fields]
    if missing:
        print("  NOTE: not on the survey layer, skipped: %s" % ", ".join(sorted(missing)))
    send = {k: v for k, v in values.items() if k.lower() in fields}
    if not send:
        raise RuntimeError("none of the accuracy fields exist on the survey layer - "
                           "add acc_points, acc_polygons, acc_avg, acc_worst, "
                           "acc_within3 and republish the survey")

    send[oidf] = oid

    # applyEdits takes adds / updates / deletes. It does NOT take `features` -
    # that is addFeatures. Passing the wrong one gets a bare 400 "Invalid
    # parameters" with an empty message, which says nothing about which
    # parameter, so it is worth being precise here.
    payload = {"f": "json", "token": token,
               "updates": json.dumps([{"attributes": send}]),
               "rollbackOnFailure": "true"}
    r = requests.post(SURVEY_URL + "/applyEdits", data=payload, timeout=60)
    r.raise_for_status()
    body = r.json()

    if "error" in body:
        err = body["error"]
        detail = "; ".join(err.get("details") or []) or err.get("message") or ""
        raise RuntimeError("update rejected (%s) %s" % (err.get("code"), detail))

    results = body.get("updateResults", [])
    if not results:
        raise RuntimeError("update returned no result: %s" % body)
    if not results[0].get("success"):
        e = results[0].get("error", {})
        raise RuntimeError("update failed on %s %s: %s"
                           % (oidf, oid, e.get("description", e)))


# ---------------------------------------------------------------------------
# Web map extent
# ---------------------------------------------------------------------------

def record_positions(token, t0, t1, job):
    """Every record's position for the day, as (lon, lat).

    Points give their own geometry; polygons give a centroid, which is all the
    framing needs and far less to transfer than their rings.
    """
    pts = []
    for url, centroid in ((POINTS_URL, False), (POLYS_URL, True)):
        params = {
            "where": date_where(url, token, "treatment_time", t0, t1)
                     + job_clause(url, token, job),
            "outFields": "OBJECTID", "outSR": 4326, "returnGeometry": "true",
        }
        if centroid:
            params["returnCentroid"] = "true"
            params["returnGeometry"] = "false"
        try:
            body = query(url, token, params)
        except RuntimeError:
            continue
        for f in body.get("features", []):
            g = f.get("centroid") if centroid else f.get("geometry")
            if g and g.get("x") is not None:
                pts.append((g["x"], g["y"]))
    return pts


def percentile(sorted_vals, pct):
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * (pct / 100.0)
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def shortest_interval(sorted_vals, pct):
    """The NARROWEST interval holding pct% of the values.

    A symmetric percentile trim is the obvious thing and it is the wrong thing
    here. Trimming 1% off each end of 256 records drops two or three points, so
    a dozen pulls made on the drive in still set the frame and the day's actual
    work stays a blob in the corner. The strays are also rarely symmetric -
    they are usually all on the way to site, so all at one end, and a symmetric
    trim spends half its budget on the end that has nothing to trim.

    Sliding a fixed-width window instead asks the question that matters: where
    are these records densest? Outliers are dropped from whichever end they are
    actually on, and a genuinely spread-out day yields a wide interval, as it
    should.
    """
    n = len(sorted_vals)
    if n == 0:
        return None, None
    k = max(2, int(math.ceil(n * pct / 100.0)))
    if k >= n:
        return sorted_vals[0], sorted_vals[-1]
    best = None
    for i in range(0, n - k + 1):
        span = sorted_vals[i + k - 1] - sorted_vals[i]
        if best is None or span < best[0]:
            best = (span, sorted_vals[i], sorted_vals[i + k - 1])
    return best[1], best[2]


def day_extent(token, t0, t1, job=None, focus=None):
    """Bounding box of the day's work, in WGS84, with a margin.

    focus=None frames everything. focus=90 frames the tightest box holding 90%
    of the records on each axis, which is what you want when a handful of pulls
    made on the way to site stretch the frame across twenty kilometres and
    squash the real work into one corner.

    Anything outside the frame is still counted in the accuracy figures; only
    the picture is narrowed, and the caller is told how many records that hides.
    """
    if focus is not None:
        pts = record_positions(token, t0, t1, job)
        if len(pts) < 5:
            focus = None            # too few for this to mean anything
        else:
            xs = sorted(p[0] for p in pts)
            ys = sorted(p[1] for p in pts)
            xmin, xmax = shortest_interval(xs, focus)
            ymin, ymax = shortest_interval(ys, focus)
            inside = sum(1 for (px, py) in pts
                         if xmin <= px <= xmax and ymin <= py <= ymax)
            print("  Framing on the tightest %g%% of %d records - %d land inside "
                  "the frame, %d fall outside."
                  % (focus, len(pts), inside, len(pts) - inside))
            # What the day looks like at a few coverage levels, so a frame that
            # comes out wrong is diagnosable without another round trip.
            lat_mid = (ys[0] + ys[-1]) / 2.0
            kx = 111320.0 * math.cos(math.radians(lat_mid))
            bits = []
            for p in (100, 95, 90, 80, 60):
                a, b = shortest_interval(xs, p)
                c, e = shortest_interval(ys, p)
                bits.append("%d%%: %.2f x %.2f km"
                            % (p, (b - a) * kx / 1000.0, (e - c) * 110570.0 / 1000.0))
            print("  Spread   " + "   ".join(bits))

    if focus is None:
        boxes = []
        for url in (POINTS_URL, POLYS_URL):
            body = query(url, token, {
                "where": date_where(url, token, "treatment_time", t0, t1)
                         + job_clause(url, token, job),
                "returnExtentOnly": "true", "outSR": 4326})
            e = body.get("extent")
            if e and e.get("xmin") is not None:
                boxes.append(e)
        if not boxes:
            return None
        xmin = min(b["xmin"] for b in boxes); xmax = max(b["xmax"] for b in boxes)
        ymin = min(b["ymin"] for b in boxes); ymax = max(b["ymax"] for b in boxes)

    # A day spent on one plant gives a zero-width box, which would zoom to the
    # centre of nowhere. Floor the span before padding it.
    if xmax - xmin < MIN_SPAN_DEG:
        mid = (xmax + xmin) / 2.0
        xmin, xmax = mid - MIN_SPAN_DEG / 2, mid + MIN_SPAN_DEG / 2
    if ymax - ymin < MIN_SPAN_DEG:
        mid = (ymax + ymin) / 2.0
        ymin, ymax = mid - MIN_SPAN_DEG / 2, mid + MIN_SPAN_DEG / 2

    padx = (xmax - xmin) * EXTENT_MARGIN
    pady = (ymax - ymin) * EXTENT_MARGIN
    xmin, xmax = xmin - padx, xmax + padx
    ymin, ymax = ymin - pady, ymax + pady

    # Match the extent's shape to the map IMAGE's shape.
    #
    # The report draws the map at 600x400 - landscape, 3:2. A day worked up and
    # down a creek gives a tall, narrow bounding box. Hand the renderer a
    # portrait extent for a landscape image and it fits one axis and crops the
    # other, which is how a correctly-calculated extent still put points off
    # the top and bottom of the picture.
    #
    # Only ever GROW the box, never shrink it, so nothing can be cropped.
    # Degrees of longitude are shorter than degrees of latitude, so the
    # comparison has to happen in metres or the correction goes the wrong way
    # at this latitude by about 20%.
    lat_mid = (ymin + ymax) / 2.0
    kx = math.cos(math.radians(lat_mid))          # lon degrees -> lat-degree equivalent
    w = (xmax - xmin) * kx
    h = (ymax - ymin)
    if h > 0 and w > 0:
        if w / h < MAP_ASPECT:                    # too tall: widen it
            need = h * MAP_ASPECT / kx
            mid = (xmin + xmax) / 2.0
            xmin, xmax = mid - need / 2.0, mid + need / 2.0
        elif w / h > MAP_ASPECT:                  # too wide: heighten it
            need = w / MAP_ASPECT
            mid = (ymin + ymax) / 2.0
            ymin, ymax = mid - need / 2.0, mid + need / 2.0

    return dict(xmin=xmin, ymin=ymin, xmax=xmax, ymax=ymax)


def portal_get(path, token, **params):
    p = {"f": "json", "token": token}
    p.update(params)
    r = requests.get(PORTAL + "/sharing/rest/" + path, params=p, timeout=60)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError("portal: %s" % body["error"].get("message", body["error"]))
    return body


# Only these services get a day filter applied. Anything else in the web map -
# a reference layer, someone else's data - is left exactly as it is.
OUR_SERVICES = ("MSA_Weed_Spray_Points", "MSA_Weed_Spray_Polygons",
                "MSA_Weed_Spray_Track")


def day_definition_expression(t0, t1):
    fmt = "%Y-%m-%d %H:%M:%S"
    a = datetime.fromtimestamp(t0 / 1000.0, timezone.utc).strftime(fmt)
    b = datetime.fromtimestamp(t1 / 1000.0, timezone.utc).strftime(fmt)
    return "treatment_time >= TIMESTAMP '%s' AND treatment_time < TIMESTAMP '%s'" % (a, b)


def retarget_layer_filters(data, t0, t1):
    """Point the EcoSync layers at the day being reported.

    The web map carried a ROLLING filter:

        treatment_time BETWEEN CURRENT_TIMESTAMP - 2 AND CURRENT_TIMESTAMP

    which works perfectly for a report generated the same day and silently
    empties any report generated later. A report for the 23rd run on the 27th
    produced a correctly framed map with nothing drawn on it - the failure is
    invisible unless you happen to know what should have been there.

    Anything in the expression that is NOT a time clause is preserved, so a
    filter splitting layers by control method survives. Brackets defeat the
    naive split, so those are replaced wholesale and said so out loud.
    """
    newclause = day_definition_expression(t0, t1)
    changed = []
    for L in data.get("operationalLayers", []):
        url = L.get("url") or ""
        if not any(s in url for s in OUR_SERVICES):
            continue
        ld = L.get("layerDefinition")
        if not isinstance(ld, dict):
            ld = {}
            L["layerDefinition"] = ld

        old = (ld.get("definitionExpression") or "").strip()
        keep = []
        if old and "(" not in old and ")" not in old:
            for part in old.split(" AND "):
                p = part.strip()
                if not p:
                    continue
                if "treatment_time" in p or "CURRENT_TIMESTAMP" in p:
                    continue
                keep.append(p)
        elif old:
            print("  NOTE: %s has a bracketed filter; replacing it wholesale."
                  % L.get("title"))
            print("        was: %s" % old)

        ld["definitionExpression"] = " AND ".join(keep + [newclause])
        changed.append((L.get("title"), old, ld["definitionExpression"]))
    return changed


def day_job_numbers(token, t0, t1):
    """Which job(s) that day's records belong to."""
    jobs = set()
    for url in (POINTS_URL, POLYS_URL):
        try:
            body = query(url, token, {
                "where": date_where(url, token, "treatment_time", t0, t1),
                "outFields": "Job_Number", "returnGeometry": "false",
                "groupByFieldsForStatistics": "Job_Number",
                "outStatistics": json.dumps([
                    {"statisticType": "count", "onStatisticField": "Job_Number",
                     "outStatisticFieldName": "n"}])})
            for f in body.get("features", []):
                v = attr(f.get("attributes", {}), "Job_Number")
                if v:
                    jobs.add(str(v).strip())
        except Exception:
            pass
    return sorted(jobs)


def choose_webmap(cfg, token, t0, t1, explicit, day_str, job=None):
    """Pick the web map from the day's job number.

    Each project has its own template with its own map, so working on the wrong
    one produces a report that looks fine and is framed on another job's ground.
    That is not a mistake anyone catches by reading the output, so it should not
    depend on remembering a flag - the data already says which job the day was.

    Add a "webmaps" block to spray_stats_config.json:

        "webmaps": {
          "DEW NP Weed Control": "997683eecc8e43c4974108e364d2eda2",
          "default":             "d7f8b5123e1b48488faf8b082f4e2393"
        }
    """
    if explicit:
        return explicit, "given with --webmap"

    maps = cfg.get("webmaps") or {}
    jobs = [job] if job else day_job_numbers(token, t0, t1)

    if not maps:
        return WEBMAP_ITEM_ID, "the built-in default (no \"webmaps\" in your config)"

    if len(jobs) == 1 and jobs[0] in maps:
        return maps[jobs[0]], "job %r" % jobs[0]

    if len(jobs) > 1:
        print("\n  ** %s has records from more than one job:" % day_str)
        for j in jobs:
            print("       %s" % j)
        print()
        print("     A day with two jobs is two reports, and everything here -")
        print("     the accuracy figures, the map extent, which survey record")
        print("     gets written - has to be scoped to one of them. Run it once")
        print("     per job:")
        print()
        for j in jobs:
            print('       spray_stats.bat %s --job "%s" --setextent --write' % (day_str, j))
        print()
        sys.exit(1)

    if jobs:
        print("\n  ** Job %r is not in the \"webmaps\" block of your config, so I "
              "cannot tell which web map this day belongs to." % jobs[0])
        print("     Add it, or pass --webmap <itemid>. **")
        sys.exit(1)

    return maps.get("default", WEBMAP_ITEM_ID), "the configured default"


def check_webmap(token, t0, t1, day_str, job=None):
    """Will the report's map actually DRAW this day's data?

    Runs on every invocation, because the ways a map comes out blank are all
    silent. A report with an empty map looks like a report; nothing on the page
    says the layer filter excluded everything, or that the renderer has no
    symbol for the species you sprayed.

    The filter test is empirical rather than a SQL parse: ask the layer for a
    count using ITS OWN definition expression AND the day, and compare against
    the day alone. If the first is zero and the second is not, the filter is
    what is hiding the data.
    """
    try:
        data = portal_get("content/items/%s/data" % WEBMAP_ITEM_ID, token)
    except Exception as e:
        print("\n  (could not read the web map to check it: %s)" % e)
        return

    layers = [L for L in data.get("operationalLayers", [])
              if any(s in (L.get("url") or "") for s in OUR_SERVICES)]
    if not layers:
        print("\n  ** The web map contains none of the EcoSync layers. **")
        return

    problems = []
    for L in layers:
        url = L.get("url")
        title = L.get("title") or url
        defexpr = ((L.get("layerDefinition") or {}).get("definitionExpression") or "").strip()
        day = date_where(url, token, "treatment_time", t0, t1) + job_clause(url, token, job)

        try:
            day_only = query(url, token, {"where": day, "returnCountOnly": "true"})["count"]
            with_filter = day_only
            if defexpr:
                with_filter = query(url, token, {
                    "where": "(%s) AND (%s)" % (defexpr, day),
                    "returnCountOnly": "true"})["count"]
        except Exception as e:
            print("  (could not test %s: %s)" % (title, e))
            continue

        if day_only == 0:
            continue                     # nothing that day; not the map's fault
        if with_filter == 0:
            problems.append("%s: %d records that day, 0 get past its filter" % (title, day_only))
        elif with_filter < day_only:
            problems.append("%s: %d of %d records get past its filter"
                            % (title, with_filter, day_only))

        # A uniqueValue renderer with no default symbol draws NOTHING for a
        # value it does not list. Golden Dodder is the live example.
        r = ((L.get("layerDefinition") or {}).get("drawingInfo") or {}).get("renderer") or {}
        if r.get("type") == "uniqueValue" and not r.get("defaultSymbol"):
            field = r.get("field1") or r.get("field")
            known = {str(u.get("value")) for u in r.get("uniqueValueInfos", [])}
            if field:
                try:
                    body = query(url, token, {
                        "where": day, "returnGeometry": "false",
                        "outFields": field, "groupByFieldsForStatistics": field,
                        "outStatistics": json.dumps([
                            {"statisticType": "count", "onStatisticField": field,
                             "outStatisticFieldName": "n"}])})
                    for f in body.get("features", []):
                        a = f.get("attributes", {})
                        raw = attr(a, field)
                        val = str(raw)
                        if val not in known:
                            problems.append("%s: %s %s has no symbol and there is no "
                                            "default, so those %s records draw nothing"
                                            % (title, field,
                                               "(blank)" if raw in (None, "") else "'%s'" % val,
                                               a.get("n")))
                except Exception:
                    pass

    print()
    if problems:
        print("  ** The report map will not show everything for %s: **" % day_str)
        for p in problems:
            print("     - %s" % p)
        print("     Run with --setextent to retarget the day filters.")
    else:
        print("  Web map check: every layer will draw this day's records.")


def set_webmap_extent(token, ext, day_str, apply_it, t0=None, t1=None):
    """Point the report's web map at the day's data.

    Nothing is changed unless apply_it is true, and the current web map JSON is
    written to a backup file first either way. This edits a live item - the
    backup is the undo.
    """
    item = portal_get("content/items/%s" % WEBMAP_ITEM_ID, token)
    data = portal_get("content/items/%s/data" % WEBMAP_ITEM_ID, token)
    owner = item.get("owner")

    # Backups live in their own folder. One per run adds up, and a tools folder
    # you cannot find spray_stats.py in is a tools folder you stop trusting.
    backup_dir = os.path.join(HERE, "webmap_backups")
    os.makedirs(backup_dir, exist_ok=True)
    backup = os.path.join(backup_dir, "webmap_backup_%s_%s.json"
                          % (WEBMAP_ITEM_ID, datetime.now().strftime("%Y%m%d-%H%M%S")))
    with open(backup, "w") as f:
        json.dump({"item": item, "data": data}, f, indent=2)

    print("\n  Web map: %s (owner %s)" % (item.get("title"), owner))
    print("  Backup written: webmap_backups/%s" % os.path.basename(backup))

    old = item.get("extent")
    print("  Current item extent: %s" % old)
    print("  New extent for %s  : [[%.6f, %.6f], [%.6f, %.6f]]"
          % (day_str, ext["xmin"], ext["ymin"], ext["xmax"], ext["ymax"]))

    # Two places can hold the opening view. Older web maps use only the item's
    # extent; ones saved in the newer Map Viewer also carry initialState. Set
    # whichever are present so the result does not depend on which viewer last
    # touched it.
    touched = ["item extent"]
    vp = data.get("initialState", {}).get("viewpoint", {})
    if "targetGeometry" in vp:
        vp["targetGeometry"] = {"xmin": ext["xmin"], "ymin": ext["ymin"],
                                "xmax": ext["xmax"], "ymax": ext["ymax"],
                                "spatialReference": {"wkid": 4326}}
        # Drop any saved scale. A viewpoint carrying BOTH a scale and an
        # envelope is ambiguous, and a stale scale left beside a fresh envelope
        # is a good way to get the right place at the wrong zoom - which is
        # exactly what the 24th's report did.
        if vp.pop("scale", None) is not None:
            print("  (cleared the web map's saved scale so the extent governs)")
        touched.append("initialState.viewpoint")

    # Framing the map is only half the job - the layers also have to be showing
    # the right day.
    changed = retarget_layer_filters(data, t0, t1) if t0 is not None else []
    for title, old, new in changed:
        print("  Layer filter: %s" % title)
        print("            was: %s" % (old or "(none)"))
        print("            now: %s" % new)
    if changed:
        touched.append("%d layer filter%s" % (len(changed), "" if len(changed) == 1 else "s"))

    # Say what ISN'T there. A polygon layer missing from the web map is the
    # kind of thing that never announces itself - the report just quietly
    # leaves the treated areas off.
    present = set()
    for L in data.get("operationalLayers", []):
        for s in OUR_SERVICES:
            if s in (L.get("url") or ""):
                present.add(s)
    for s in OUR_SERVICES:
        if s not in present:
            print("  NOTE: %s is not in this web map, so it cannot appear on the report."
                  % s)

    print("  Will update: %s" % ", ".join(touched))

    if not apply_it:
        print("\n  Dry run - nothing changed. Add --setextent to apply.")
        return

    if not owner:
        raise RuntimeError("could not determine the web map's owner; "
                           "--setextent needs username/password, not an api_key")

    payload = {
        "f": "json", "token": token,
        "extent": "%.6f,%.6f,%.6f,%.6f" % (ext["xmin"], ext["ymin"],
                                           ext["xmax"], ext["ymax"]),
    }
    if "initialState.viewpoint" in touched or changed:
        payload["text"] = json.dumps(data)

    r = requests.post(
        "%s/sharing/rest/content/users/%s/items/%s/update" % (PORTAL, owner, WEBMAP_ITEM_ID),
        data=payload, timeout=60)
    r.raise_for_status()
    body = r.json()
    if not body.get("success"):
        raise RuntimeError("web map update failed: %s"
                           % body.get("error", {}).get("message", body))
    print("\n  Web map repositioned. Generate the report - it should fit first time.")


def print_legend(token, item_id):
    """Every symbol class in a web map, with its hex colour.

    This exists to keep the report template's legend honest. The template's
    swatches are shaded table cells set by hand, so the only thing stopping
    them drifting from the map is being able to read the map's actual colours
    without squinting at a screenshot - which is how the old legend ended up
    showing a species that was no longer symbolised and mislabelling one that
    was.
    """
    data = portal_get("content/items/%s/data" % item_id, token)
    item = portal_get("content/items/%s" % item_id, token)
    print("\nLegend for: %s  (%s)\n" % (item.get("title"), item_id))

    def hexof(c):
        if not c:
            return "(no fill)"
        s = "#%02X%02X%02X" % (c[0], c[1], c[2])
        if len(c) > 3 and c[3] < 255:
            s += "  alpha %d" % c[3]
        return s

    def describe(sym, indent):
        if not sym:
            return
        kind = {"esriSMS": "point marker", "esriSFS": "polygon fill",
                "esriSLS": "line", "esriPMS": "picture marker",
                "CIMSymbolReference": "CIM symbol"}.get(sym.get("type"),
                                                        sym.get("type", "?"))
        fill = hexof(sym.get("color"))
        line = sym.get("outline") or {}
        extra = ""
        if line.get("color"):
            extra = "   outline %s width %s" % (hexof(line.get("color")), line.get("width"))
        print("%s%-22s %s%s" % (indent, kind.replace("esriS", ""), fill, extra))

    for L in data.get("operationalLayers", []):
        url = L.get("url") or ""
        if not any(s in url for s in OUR_SERVICES):
            continue
        print("  LAYER: %s" % L.get("title"))
        r = ((L.get("layerDefinition") or {}).get("drawingInfo") or {}).get("renderer") or {}
        rtype = r.get("type")
        if rtype == "uniqueValue":
            field = r.get("field1") or r.get("field")
            print("    symbolised on: %s" % field)
            for u in r.get("uniqueValueInfos", []):
                print("    value %-24r label %r" % (u.get("value"), u.get("label")))
                describe(u.get("symbol"), "        ")
            if r.get("defaultSymbol"):
                print("    DEFAULT (anything not listed above)")
                describe(r.get("defaultSymbol"), "        ")
            else:
                print("    no default symbol - unlisted values draw NOTHING")
        elif rtype == "simple":
            print("    single symbol, label %r" % r.get("label"))
            describe(r.get("symbol"), "        ")
        else:
            print("    renderer type %r - not summarised here" % rtype)
        print()


def main():
    args = [a for a in sys.argv[1:]]
    do_write = "--write" in args
    do_extent = "--setextent" in args
    show_extent = do_extent or "--showextent" in args
    want_legend = "--legend" in args

    # --webmap <itemid> picks which project's web map to work on. Each project
    # has its own template with its own map expression, so it needs its own
    # framing and its own day filter; without this the DEW NP map would never
    # be touched and its reports would come out framed on whatever the boxthorn
    # map was last set to.
    # --mapsize 600x400, matching the template's size: argument.
    global MAP_ASPECT
    if "--mapsize" in args:
        k = args.index("--mapsize")
        try:
            w, h = args[k + 1].lower().split("x")
            MAP_ASPECT = float(w) / float(h)
        except Exception:
            print("--mapsize wants WIDTHxHEIGHT, e.g. --mapsize 600x400")
            sys.exit(1)
        del args[k:k + 2]

    focus = None
    if "--focus" in args:
        k = args.index("--focus")
        focus = 98.0
        if k + 1 < len(args):
            try:
                focus = float(args[k + 1]); del args[k + 1]
            except ValueError:
                pass
        del args[k]
        if not (50.0 <= focus <= 100.0):
            print("--focus wants a percentage between 50 and 100.")
            sys.exit(1)

    manual_extent = None
    if "--extent" in args:
        k = args.index("--extent")
        try:
            v = [float(n) for n in args[k + 1].split(",")]
            assert len(v) == 4
            manual_extent = dict(xmin=v[0], ymin=v[1], xmax=v[2], ymax=v[3])
            del args[k:k + 2]
        except Exception:
            print("--extent wants xmin,ymin,xmax,ymax in decimal degrees.")
            sys.exit(1)

    job = None
    if "--job" in args:
        k = args.index("--job")
        if k + 1 >= len(args):
            print('--job needs a job number after it, e.g. --job "DEW NP Weed Control"')
            sys.exit(1)
        job = args[k + 1]
        del args[k:k + 2]

    explicit_webmap = None
    if "--webmap" in args:
        k = args.index("--webmap")
        if k + 1 >= len(args):
            print("--webmap needs a web map item id after it.")
            sys.exit(1)
        explicit_webmap = args[k + 1]
        del args[k:k + 2]
    global WEBMAP_ITEM_ID
    if explicit_webmap:
        WEBMAP_ITEM_ID = explicit_webmap

    args = [a for a in args if not a.startswith("--")]

    tz = ZoneInfo(LOCAL_TZ)

    if want_legend:
        # An item id may also follow --legend directly.
        item_id = args[0] if args and len(args[0]) == 32 else WEBMAP_ITEM_ID
        cfg = load_config()
        print_legend(get_token(cfg), item_id)
        return

    day_str = args[0] if args else datetime.now(tz).strftime("%Y-%m-%d")

    cfg = load_config()
    token = get_token(cfg)
    t0, t1 = day_window_ms(day_str)

    print("\nMSA EcoSync - positional data quality for %s (%s)\n" % (day_str, LOCAL_TZ))

    WEBMAP_ITEM_ID, why = choose_webmap(cfg, token, t0, t1, explicit_webmap, day_str, job)
    if job:
        print("  Job:     %s  (everything below is scoped to this job)" % job)
    print("  Web map: %s  (chosen from %s)\n" % (WEBMAP_ITEM_ID, why))

    pts = layer_stats(POINTS_URL, token, t0, t1, job)
    pol = layer_stats(POLYS_URL, token, t0, t1, job)

    captured = pts["total"] + pol["total"]
    if captured == 0:
        print("  No records found for that date. Has the unit synced?\n")
        return

    n = pts["n"] + pol["n"]
    if n == 0:
        print("  %d records, but none carry gps_accuracy_m.\n" % captured)
        return

    avg = (pts["sum"] + pol["sum"]) / n
    worst = max(w for w in (pts["worst"], pol["worst"]) if w is not None)
    # Floor, not round.
    #
    # 789 of 790 records within 3 m is 99.87%, and round() turns that into a
    # report that says 100% while "worst recorded" on the line above says
    # 3.1 m. The two lines contradict each other, and the one that overstates
    # is the one a compliance reader will quote. Flooring can only ever
    # understate, which is the safe direction for a claim about our own work.
    within = pts["within"] + pol["within"]
    within_pct = int(math.floor(100.0 * within / n))
    # ...but never report 99% for a day where every record actually passed.
    if within == n:
        within_pct = 100

    values = {
        "acc_points":   pts["total"],
        "acc_polygons": pol["total"],
        "acc_avg":      round(avg, 1),
        "acc_worst":    round(worst, 1),
        "acc_within3":  within_pct,
    }

    print("  Records captured:   %d points, %d polygons" % (values["acc_points"], values["acc_polygons"]))
    print("  Average accuracy:   %.1f m" % values["acc_avg"])
    print("  Worst recorded:     %.1f m" % values["acc_worst"])
    print("  Within %.0f m:          %d%%" % (ACCURACY_THRESHOLD_M, values["acc_within3"]))
    if n < captured:
        print("\n  (%d of %d records have no accuracy value and were left out of "
              "the averages.)" % (captured - n, captured))

    # Always, whatever flags were given. The day filter has nothing to do with
    # the extent, and hiding the check behind --setextent meant a plain --write
    # run reported healthy figures for a map that was going to come out blank.
    check_webmap(token, t0, t1, day_str, job)

    if show_extent:
        ext = manual_extent or day_extent(token, t0, t1, job, focus)
        if ext is None:
            print("\n  No geometry found for that date - extent unchanged.")
        else:
            set_webmap_extent(token, ext, day_str, do_extent, t0, t1)

    if not do_write:
        print("\n  Accuracy figures are print-only. Add --write to put them into "
              "the Survey123 record.\n")
        return

    oid, found_job = find_survey_record(token, day_str, job)
    if oid is None:
        print("\n  No Survey123 record found for %s - submit the form first, "
              "then run this again.\n" % day_str)
        return

    write_back(token, oid, values)
    print("\n  Written to Survey123 record %s%s.\n"
          % (oid, (" (job %s)" % found_job) if found_job else ""))


if __name__ == "__main__":
    try:
        main()
    except requests.HTTPError as e:
        print("\nNetwork or server error: %s\n" % e)
        sys.exit(1)
    except RuntimeError as e:
        print("\n%s\n" % e)
        sys.exit(1)
