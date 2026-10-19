#!/usr/bin/env python3
"""
MSA EcoSync - end of field day.

Replaces the Runs.txt checklist. Works out for itself which jobs the day's
records belong to, then runs the polygon build and the per-job statistics in
the right order.

    python end_of_day.py                 today, dry run - shows what it would do
    python end_of_day.py --write         today, for real
    python end_of_day.py 2026-10-14      a given day, dry run
    python end_of_day.py --yesterday     for when you process the next morning

Options
    --write        actually post polygons and write the stats back
    --no-extent    skip --setextent on the stats run
    --no-track     skip the polygon build
    --no-stats     skip the statistics run

Nothing is written without --write, matching spray_stats.py and
track_to_polygons.py. The dry run is safe to use as a look-before-you-leap.

Why this exists: the old routine had a judgement step in it - "if the day has
records from more than one job, run it once per job". Forgetting the second job
meant that job's report came out framed on the wrong data, and nothing said so.
The job list is now read from the layer rather than from memory.
"""

import os
import sys
import subprocess
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# spray_stats guards main() behind __main__, so importing it is safe and gives
# us its config loading, token handling and job discovery rather than a second
# copy of all three.
import spray_stats as ss  # noqa: E402

try:
    from zoneinfo import ZoneInfo
except ImportError:                                   # pragma: no cover
    from backports.zoneinfo import ZoneInfo           # type: ignore

LOG_PATH = os.path.join(HERE, "end_of_day_log.txt")


# ---------------------------------------------------------------------------

def resolve_day(args):
    """A bare date argument wins; otherwise today, or yesterday on request."""
    tz = ZoneInfo(ss.LOCAL_TZ)
    for a in args:
        if not a.startswith("--"):
            return a
    now = datetime.now(tz)
    if "--yesterday" in args:
        now -= timedelta(days=1)
    return now.strftime("%Y-%m-%d")


def capture_split(token, t0, t1):
    """How many of the day's points came from a unit, and how many from a form.

    The weed points layer carries both EcoSync device records and Survey123
    entries. Only the device populates capture_mode, so it is the discriminator.
    Returns (device, form) or (None, None) if the field is absent.
    """
    if not ss.field_named(ss.POINTS_URL, token, "capture_mode"):
        return None, None
    try:
        body = ss.query(ss.POINTS_URL, token, {
            "where": ss.date_where(ss.POINTS_URL, token, "treatment_time", t0, t1),
            "outFields": "capture_mode",
            "returnGeometry": "false",
            "groupByFieldsForStatistics": "capture_mode",
            "outStatistics": ss.json.dumps([
                {"statisticType": "count",
                 "onStatisticField": "capture_mode",
                 "outStatisticFieldName": "n"}])})
    except Exception:
        return None, None

    device = form = 0
    for f in body.get("features", []):
        a = f.get("attributes", {})
        mode = ss.attr(a, "capture_mode")
        n = ss.attr(a, "n") or 0
        if mode:
            device += n
        else:
            form += n

    # A grouped count can omit the null bucket entirely, so get the true total
    # separately and treat the remainder as form entries.
    try:
        total = ss.query(ss.POINTS_URL, token, {
            "where": ss.date_where(ss.POINTS_URL, token, "treatment_time", t0, t1),
            "returnCountOnly": "true"}).get("count", device + form)
        form = max(0, total - device)
    except Exception:
        pass

    return device, form


def run(script, script_args, label):
    """Run one of the sibling scripts, streaming its output straight through."""
    cmd = [sys.executable, os.path.join(HERE, script)] + script_args
    print()
    print("=" * 74)
    print(" %s" % label)
    print(" %s" % " ".join(('"%s"' % a if " " in a else a) for a in cmd[1:]))
    print("=" * 74)
    sys.stdout.flush()
    rc = subprocess.call(cmd, cwd=HERE)
    if rc != 0:
        print("\n  ** %s exited with code %d **" % (script, rc))
    return rc


def log(line):
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line.rstrip() + "\n")
    except OSError:
        pass


# ---------------------------------------------------------------------------

def main():
    args = sys.argv[1:]
    do_write = "--write" in args
    do_track = "--no-track" not in args
    do_stats = "--no-stats" not in args
    set_extent = "--no-extent" not in args

    day = resolve_day(args)

    print()
    print("MSA EcoSync - end of day for %s (%s)" % (day, ss.LOCAL_TZ))
    print("Mode: %s" % ("WRITE - this will post polygons and update records"
                        if do_write else
                        "DRY RUN - nothing will be written. Add --write when happy."))

    cfg = ss.load_config()
    token = ss.get_token(cfg)
    t0, t1 = ss.day_window_ms(day)

    jobs = ss.day_job_numbers(token, t0, t1)
    device, form = capture_split(token, t0, t1)

    print()
    if device is not None:
        print("  Point records : %d from EcoSync units, %d from Survey123" % (device, form))
    if jobs:
        print("  Jobs that day : %d" % len(jobs))
        for j in jobs:
            print("      - %s" % j)
    else:
        print("  Jobs that day : none found")
        print("                  Either nothing has synced yet, or the records")
        print("                  carry no Job_Number. Check before writing.")

    if not jobs and not device:
        print()
        print("  Nothing to process for %s. Has the unit synced?" % day)
        print()
        return 0

    failures = []

    if do_track:
        a = [day] + (["--write"] if do_write else [])
        if run("track_to_polygons.py", a, "Treated-area polygons") != 0:
            failures.append("track_to_polygons")

    if do_stats:
        if jobs:
            for j in jobs:
                a = [day, "--job", j]
                if set_extent:
                    a.append("--setextent")
                if do_write:
                    a.append("--write")
                if run("spray_stats.py", a, "Statistics - %s" % j) != 0:
                    failures.append("spray_stats (%s)" % j)
        else:
            a = [day] + (["--setextent"] if set_extent else []) + \
                (["--write"] if do_write else [])
            if run("spray_stats.py", a, "Statistics - all records") != 0:
                failures.append("spray_stats")

    print()
    print("=" * 74)
    if failures:
        print(" FINISHED WITH PROBLEMS: %s" % ", ".join(failures))
        print(" Read the output above before relying on today's figures.")
    else:
        print(" Finished.%s" % ("" if do_write else
                                "  Dry run - re-run with --write to commit."))
    print("=" * 74)
    print()

    log("%s  day=%s  mode=%s  jobs=%d  device=%s  form=%s  %s"
        % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), day,
           "write" if do_write else "dry",
           len(jobs),
           device if device is not None else "?",
           form if form is not None else "?",
           ("FAILED: " + ", ".join(failures)) if failures else "ok"))

    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped.\n")
        sys.exit(130)
    except RuntimeError as e:
        print("\n%s\n" % e)
        sys.exit(1)
