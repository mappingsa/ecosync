"""Copy EcoSyncField records from the ArcGIS spray layers into the EcoSync
Data Platform (Supabase).

  python import_arcgis.py            last 14 days (the hourly run)
  python import_arcgis.py --days 60  a longer window
  python import_arcgis.py --full     everything (first run / history)
  python import_arcgis.py --dry-run  read and match, write nothing

Safe to run as often as you like: every record is keyed by its ArcGIS
GlobalID, so re-running updates rather than duplicates. ArcGIS stays the
source of truth during the parallel run - edit there, not in the platform.

Sign-in:
  ArcGIS   - the same spray_stats_config.json End of Day uses (EcoSync\\tools).
  Supabase - .env next to this script (copy .env.example). The service key
             bypasses all security: keep it in .env only, never in git.
"""

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone

import requests

from mapping import attr, build_index, esri_to_ewkt, match, track_runs

HERE = os.path.dirname(os.path.abspath(__file__))
ARCGIS_CONFIG = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "spray_stats_config.json"))
ENV_FILE = os.path.join(HERE, ".env")
LOG_FILE = os.path.join(HERE, "import_log.txt")
ORG_NAME = "FieldPoint Environmental"

SERVICES = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services"
LAYERS = {
    "points":   ("%s/MSA_Weed_Spray_Points/FeatureServer/0" % SERVICES, "treatment_points"),
    "lines":    ("%s/Weed_Spray_Lines/FeatureServer/0" % SERVICES, "treatment_lines"),
    "polygons": ("%s/MSA_Weed_Spray_Polygons/FeatureServer/0" % SERVICES, "treatment_areas"),
}
TRACK_URL = "%s/MSA_Weed_Spray_Track/FeatureServer/0" % SERVICES
BATCH = 500


def log(msg):
    line = "%s  %s" % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# --- Settings ---------------------------------------------------------------------

def read_env():
    if not os.path.exists(ENV_FILE):
        sys.exit("No .env found next to import_arcgis.py. Copy .env.example to .env and fill in "
                 "SUPABASE_URL and SUPABASE_SERVICE_KEY (Supabase > Project Settings > API).")
    env = {}
    with open(ENV_FILE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    for k in ("SUPABASE_URL", "SUPABASE_SERVICE_KEY"):
        if not env.get(k):
            sys.exit("%s is missing from .env" % k)
    return env


def arcgis_token():
    if not os.path.exists(ARCGIS_CONFIG):
        sys.exit("ArcGIS sign-in not found at %s (the file End of Day uses)." % ARCGIS_CONFIG)
    with open(ARCGIS_CONFIG, encoding="utf-8") as f:
        cfg = json.load(f)
    if cfg.get("api_key"):
        return cfg["api_key"]
    r = requests.post("https://www.arcgis.com/sharing/rest/generateToken", timeout=30, data={
        "username": cfg.get("username", ""), "password": cfg.get("password", ""),
        "referer": "https://www.arcgis.com", "expiration": 60, "f": "json"})
    r.raise_for_status()
    body = r.json()
    if "token" not in body:
        # never echo the response: it can contain the credentials
        sys.exit("ArcGIS sign-in failed: %s" % body.get("error", {}).get("message", "unknown error"))
    return body["token"]


# --- ArcGIS -----------------------------------------------------------------------

_date_style = {}


def arc_post(url, token, params):
    p = {"f": "json", "token": token}
    p.update(params)
    r = requests.post(url, data=p, timeout=90)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError("%s: %s" % (url.rsplit("/services/", 1)[-1], body["error"].get("message", body["error"])))
    return body


def since_where(url, token, since_ms):
    """treatment_time >= since. Some services want epoch ms, some a TIMESTAMP
    literal (End of Day found the same): probe once per layer."""
    if since_ms is None:
        return "1=1"
    epoch = "treatment_time >= %d" % since_ms
    stamp = "treatment_time >= TIMESTAMP '%s'" % datetime.fromtimestamp(
        since_ms / 1000, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    if url not in _date_style:
        try:
            arc_post(url + "/query", token, {"where": epoch, "returnCountOnly": "true"})
            _date_style[url] = "epoch"
        except RuntimeError:
            _date_style[url] = "stamp"
    return epoch if _date_style[url] == "epoch" else stamp


def fetch_all(url, token, where):
    out = []
    for _ in range(1000):
        body = arc_post(url + "/query", token, {
            "where": where, "outFields": "*", "returnGeometry": "true", "outSR": "4326",
            "orderByFields": "OBJECTID", "resultOffset": str(len(out)), "resultRecordCount": "2000"})
        feats = body.get("features", [])
        out.extend(feats)
        if not body.get("exceededTransferLimit") or not feats:
            break
    return out


# --- Supabase ---------------------------------------------------------------------

class Supabase:
    def __init__(self, url, key, dry_run=False):
        self.rest = url.rstrip("/") + "/rest/v1/"
        self.h = {"apikey": key, "Content-Type": "application/json"}
        if not key.startswith("sb_"):
            # Legacy service_role keys are JWTs and also go in Authorization.
            # The newer sb_secret_ keys must only be sent as apikey.
            self.h["Authorization"] = "Bearer " + key
        self.dry_run = dry_run

    def get(self, table, params):
        r = requests.get(self.rest + table, headers=self.h, params=params, timeout=60)
        if r.status_code >= 400:
            raise RuntimeError("Supabase %s: %s" % (table, r.text[:300]))
        return r.json()

    def upsert(self, table, rows, conflict):
        if self.dry_run or not rows:
            return
        h = dict(self.h, Prefer="resolution=merge-duplicates,return=minimal")
        for i in range(0, len(rows), BATCH):
            r = requests.post(self.rest + table, headers=h, params={"on_conflict": conflict},
                              data=json.dumps(rows[i:i + BATCH]), timeout=120)
            if r.status_code >= 400:
                raise RuntimeError("Supabase %s: %s" % (table, r.text[:300]))


def iso(ms):
    return datetime.fromtimestamp(ms / 1000, timezone.utc).isoformat() if ms else None


def num(v):
    try:
        return float(v) if v not in (None, "") else None
    except (TypeError, ValueError):
        return None


# --- Import -----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=14, help="how many days back to copy (default 14)")
    ap.add_argument("--full", action="store_true", help="copy everything")
    ap.add_argument("--dry-run", action="store_true", help="read and match, write nothing")
    args = ap.parse_args()

    env = read_env()
    sb = Supabase(env["SUPABASE_URL"], env["SUPABASE_SERVICE_KEY"], args.dry_run)
    orgs = sb.get("organisations", {"name": "eq." + ORG_NAME, "select": "id"})
    if not orgs:
        sys.exit("Organisation '%s' not found - run 002_seed_fpe.sql first." % ORG_NAME)
    org = orgs[0]["id"]

    q = {"org_id": "eq." + org}
    species_rows = sb.get("species", dict(q, select="id,name,aliases,default_method_id"))
    method_rows = sb.get("control_methods", dict(q, select="id,name,aliases"))
    job_rows = sb.get("jobs", dict(q, select="id,job_number,aliases"))
    species_ix = build_index(species_rows, "name", "aliases")
    method_ix = build_index(method_rows, "name", "aliases")
    job_ix = build_index(job_rows, "job_number", "aliases")
    default_method = {r["id"]: r.get("default_method_id") for r in species_rows}

    token = arcgis_token()
    since = None if args.full else int((time.time() - args.days * 86400) * 1000)
    unmatched = {}   # (source, field, value) -> count

    def note(source, field, value):
        if value not in (None, ""):
            k = (source, field, str(value).strip())
            unmatched[k] = unmatched.get(k, 0) + 1

    totals = {}
    for name, (url, table) in LAYERS.items():
        feats = fetch_all(url, token, since_where(url, token, since))
        source = "arcgis:" + url.rsplit("/services/", 1)[-1].split("/")[0]
        rows, skipped = [], 0
        for f in feats:
            a = f.get("attributes") or {}
            geom = esri_to_ewkt(f.get("geometry"))
            when = attr(a, "treatment_time")
            if not geom or not when:
                skipped += 1
                continue
            job_raw, sp_raw, m_raw = attr(a, "Job_Number"), attr(a, "weed_species"), attr(a, "control_type")
            job_id, sp_id, m_id = match(job_raw, job_ix), match(sp_raw, species_ix), match(m_raw, method_ix)
            if job_id is None:
                note(source, "job_number", job_raw)
            if sp_id is None:
                note(source, "weed_species", sp_raw)
            if m_id is None:
                note(source, "control_type", m_raw)
                m_id = default_method.get(sp_id)   # blank method: the species' usual one
            ref = attr(a, "GlobalID") or attr(a, "OBJECTID")
            rows.append({
                "org_id": org, "job_id": job_id, "species_id": sp_id, "method_id": m_id,
                "treated_at": iso(when), "accuracy_m": num(attr(a, "gps_accuracy_m")),
                "run_id": None if attr(a, "run_id") in (None, "") else str(attr(a, "run_id")),
                "capture_mode": attr(a, "capture_mode"),
                "source": source, "source_ref": "%s:%s" % (name, ref), "geom": geom,
            })
        sb.upsert(table, rows, "org_id,source,source_ref")
        totals[name] = (len(rows), skipped)

    # Track points -> one line per run
    feats = fetch_all(TRACK_URL, token, since_where(TRACK_URL, token, since))
    runs = track_runs(feats)
    track_rows = []
    for r in runs:
        job_id = match(r["job"], job_ix)
        if job_id is None:
            note("arcgis:MSA_Weed_Spray_Track", "job_number", r["job"])
        track_rows.append({
            "org_id": org, "job_id": job_id, "started_at": iso(r["start"]), "ended_at": iso(r["end"]),
            "source": "arcgis:MSA_Weed_Spray_Track", "source_ref": "track:" + r["key"], "geom": r["ewkt"],
        })
    sb.upsert("tracks", track_rows, "org_id,source,source_ref")
    totals["track runs"] = (len(track_rows), len(feats))

    now = datetime.now(timezone.utc).isoformat()
    sb.upsert("needs_a_home", [
        {"org_id": org, "source": s, "field": fld, "value": v, "seen": n, "last_seen": now}
        for (s, fld, v), n in unmatched.items()], "org_id,source,field,value")

    window = "everything" if args.full else "last %d days" % args.days
    summary = ", ".join("%s %d" % (k, v[0]) for k, v in totals.items())
    log("%s%s: %s" % ("DRY RUN " if args.dry_run else "", window, summary))
    if any(v[1] for k, v in totals.items() if k != "track runs"):
        log("  skipped (no geometry or time): " + ", ".join("%s %d" % (k, v[1]) for k, v in totals.items() if k != "track runs" and v[1]))
    if unmatched:
        log("  not matched to a list (see needs_a_home in Supabase):")
        for (s, fld, v), n in sorted(unmatched.items(), key=lambda x: -x[1]):
            log("    %-13s %-30s x%d" % (fld, "[%s]" % v, n))


if __name__ == "__main__":
    try:
        main()
    except (requests.RequestException, RuntimeError) as e:
        log("FAILED: %s" % e)
        sys.exit(1)
