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

Credentials live in spray_stats_config.json beside this script - never in here,
and never in git. Copy spray_stats_config.example.json and fill it in.

Only needs `requests`. No ArcGIS Python API, no conda environment.
"""

import json
import os
import sys
from datetime import datetime, timedelta

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


def load_config():
    if not os.path.exists(CONFIG_PATH):
        print("No config found at:\n  %s\n" % CONFIG_PATH)
        print("Copy spray_stats_config.example.json to spray_stats_config.json "
              "and fill in your credentials.")
        sys.exit(1)
    with open(CONFIG_PATH) as f:
        return json.load(f)


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


def layer_stats(url, token, t0, t1):
    """count, sum and worst of gps_accuracy_m, plus how many are within the threshold.

    Records with a NULL accuracy are counted as captured but excluded from the
    accuracy maths - older firmware did not write the field, and averaging over
    a missing value would quietly understate the figure.
    """
    where_day = "treatment_time >= %d AND treatment_time < %d" % (t0, t1)

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


def find_survey_record(token, day_str):
    """The Survey123 record for that spray_date. Returns its OBJECTID, or None."""
    t0, t1 = day_window_ms(day_str)
    body = query(SURVEY_URL, token, {
        "where": "spray_date >= %d AND spray_date < %d" % (t0, t1),
        "outFields": "OBJECTID,job_number", "returnGeometry": "false"})
    feats = body.get("features", [])
    if not feats:
        return None, None
    if len(feats) > 1:
        print("  NOTE: %d survey records share that date - updating the first "
              "(OBJECTID %s)." % (len(feats), feats[0]["attributes"]["OBJECTID"]))
    a = feats[0]["attributes"]
    return a["OBJECTID"], a.get("job_number")


def write_back(token, oid, values):
    payload = {"f": "json", "token": token,
               "features": json.dumps([{"attributes": dict(OBJECTID=oid, **values)}])}
    r = requests.post(SURVEY_URL + "/applyEdits", data=payload, timeout=60)
    r.raise_for_status()
    body = r.json()
    results = body.get("updateResults", [])
    if not results or not results[0].get("success"):
        raise RuntimeError("update failed: %s" % body)


# ---------------------------------------------------------------------------
# Web map extent
# ---------------------------------------------------------------------------

def day_extent(token, t0, t1):
    """Bounding box of everything captured that day, in WGS84, with a margin."""
    boxes = []
    for url in (POINTS_URL, POLYS_URL):
        body = query(url, token, {
            "where": "treatment_time >= %d AND treatment_time < %d" % (t0, t1),
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
    return dict(xmin=xmin - padx, ymin=ymin - pady,
                xmax=xmax + padx, ymax=ymax + pady)


def portal_get(path, token, **params):
    p = {"f": "json", "token": token}
    p.update(params)
    r = requests.get(PORTAL + "/sharing/rest/" + path, params=p, timeout=60)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError("portal: %s" % body["error"].get("message", body["error"]))
    return body


def set_webmap_extent(token, ext, day_str, apply_it):
    """Point the report's web map at the day's data.

    Nothing is changed unless apply_it is true, and the current web map JSON is
    written to a backup file first either way. This edits a live item - the
    backup is the undo.
    """
    item = portal_get("content/items/%s" % WEBMAP_ITEM_ID, token)
    data = portal_get("content/items/%s/data" % WEBMAP_ITEM_ID, token)
    owner = item.get("owner")

    backup = os.path.join(HERE, "webmap_backup_%s_%s.json"
                          % (WEBMAP_ITEM_ID, datetime.now().strftime("%Y%m%d-%H%M%S")))
    with open(backup, "w") as f:
        json.dump({"item": item, "data": data}, f, indent=2)

    print("\n  Web map: %s (owner %s)" % (item.get("title"), owner))
    print("  Backup written: %s" % os.path.basename(backup))

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
        touched.append("initialState.viewpoint")
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
    if "initialState.viewpoint" in touched:
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


def main():
    args = [a for a in sys.argv[1:]]
    do_write = "--write" in args
    do_extent = "--setextent" in args
    show_extent = do_extent or "--showextent" in args
    args = [a for a in args if not a.startswith("--")]

    tz = ZoneInfo(LOCAL_TZ)
    day_str = args[0] if args else datetime.now(tz).strftime("%Y-%m-%d")

    cfg = load_config()
    token = get_token(cfg)
    t0, t1 = day_window_ms(day_str)

    print("\nMSA EcoSync - positional data quality for %s (%s)\n" % (day_str, LOCAL_TZ))

    pts = layer_stats(POINTS_URL, token, t0, t1)
    pol = layer_stats(POLYS_URL, token, t0, t1)

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
    within_pct = round(100.0 * (pts["within"] + pol["within"]) / n)

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

    if show_extent:
        ext = day_extent(token, t0, t1)
        if ext is None:
            print("\n  No geometry found for that date - extent unchanged.")
        else:
            set_webmap_extent(token, ext, day_str, do_extent)

    if not do_write:
        print("\n  Accuracy figures are print-only. Add --write to put them into "
              "the Survey123 record.\n")
        return

    oid, job = find_survey_record(token, day_str)
    if oid is None:
        print("\n  No Survey123 record found for %s - submit the form first, "
              "then run this again.\n" % day_str)
        return

    write_back(token, oid, values)
    print("\n  Written to Survey123 record OBJECTID %s%s.\n"
          % (oid, (" (job %s)" % job) if job else ""))


if __name__ == "__main__":
    try:
        main()
    except requests.HTTPError as e:
        print("\nNetwork or server error: %s\n" % e)
        sys.exit(1)
    except RuntimeError as e:
        print("\n%s\n" % e)
        sys.exit(1)
