#!/usr/bin/env python3
"""
MSA EcoSync - pre-flight check before the end-of-day run.

Reads only. Answers the two questions end_of_day.py does not ask until it is
already writing:

  1. Is there a Form 1 (Survey123) record for each job worked that day?
     spray_stats.py only looks for it during --write, and if it is missing it
     says so and carries on - by then the web map has already been reframed.

  2. Does each job have a web map in spray_stats_config.json?
     spray_stats.py stops on an unknown job, but only part-way through.

    python check_before_end_of_day.py               today
    python check_before_end_of_day.py 2026-10-28    a given day

Exit code 0 = all clear, 1 = something to fix first, 2 = bad date.
"""

import os
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import spray_stats as ss  # noqa: E402
from zoneinfo import ZoneInfo  # noqa: E402


def _fmt_local(ms):
    if ms is None:
        return "no date"
    return datetime.fromtimestamp(ms / 1000.0, ZoneInfo(ss.LOCAL_TZ)).strftime("%Y-%m-%d %H:%M")


def near_misses(token, day, job):
    """Show the Form 1 records that nearly match, so the reason is visible.

    The lookup needs the date AND the job number to match exactly. When it finds
    nothing, list (a) every Form 1 record dated that day, whatever its job, and
    (b) records with a similar job number in the week either side - with the job
    number in [brackets] so a stray space shows up.
    """
    url = ss.SURVEY_URL
    jf = ss.field_named(url, token, "job_number") or "job_number"
    df = ss.field_named(url, token, "spray_date") or "spray_date"
    out = "%s,%s" % (jf, df)

    t0, t1 = ss.day_window_ms(day)
    same_day = ss.query(url, token, {"where": ss.date_where(url, token, df, t0, t1),
                                     "outFields": out, "returnGeometry": "false"})
    feats = same_day.get("features", [])
    print("         Form 1 records dated %s: %d" % (day, len(feats)))
    for f in feats[:10]:
        a = f["attributes"]
        print("           job number [%s]" % ss.attr(a, jf))

    w0 = t0 - 7 * 86400000
    w1 = t1 + 7 * 86400000
    key = " ".join(job.split()[:2]).replace("'", "''").upper() or job.upper()
    where = "%s AND UPPER(%s) LIKE '%%%s%%'" % (ss.date_where(url, token, df, w0, w1), jf, key)
    try:
        near = ss.query(url, token, {"where": where, "outFields": out,
                                     "returnGeometry": "false",
                                     "orderByFields": "%s DESC" % df})
        nf = near.get("features", [])
    except Exception:
        nf = []
    if nf:
        print("         Similar job numbers within a week of %s:" % day)
        for f in nf[:10]:
            a = f["attributes"]
            print("           %s  job number [%s]" % (_fmt_local(ss.attr(a, df)), ss.attr(a, jf)))
    elif not feats:
        print("         No Form 1 record with a similar job number in the week")
        print("         either side. Is it still in the Survey123 Outbox?")


def main():
    args = [a for a in sys.argv[1:] if a.strip()]
    if args:
        day = args[0]
        try:
            datetime.strptime(day, "%Y-%m-%d")
        except ValueError:
            print("\n  '%s' is not a date. Use YYYY-MM-DD, e.g. 2026-10-28.\n" % day)
            return 2
    else:
        day = datetime.now(ZoneInfo(ss.LOCAL_TZ)).strftime("%Y-%m-%d")

    print()
    print("  Pre-flight check for %s" % day)
    print("  " + "-" * 60)

    cfg = ss.load_config()
    token = ss.get_token(cfg)
    t0, t1 = ss.day_window_ms(day)

    jobs = ss.day_job_numbers(token, t0, t1)
    if not jobs:
        print("  No job numbers found in the day's spray records.")
        print("  Has the EcoSync unit synced? Check before going on.")
        print()
        return 1

    maps = cfg.get("webmaps") or {}
    problems = 0
    for job in jobs:
        print("  Job: %s" % job)

        oid, _ = ss.find_survey_record(token, day, job)
        if oid is None:
            problems += 1
            print("     [X] No Form 1 spray record for this job on %s." % day)
            print("         Submit the Form 1 record first. The job number must")
            print("         read exactly:  [%s]" % job)
            near_misses(token, day, job)
        else:
            print("     [OK] Form 1 spray record found (record %s)." % oid)

        if maps:
            hit, _ = ss.match_map(maps, job)
            if not hit:
                problems += 1
                print("     [X] This job has no web map set up for its report.")
                print("         Stop here and tell Jayme the job name above.")
            else:
                print("     [OK] Report web map is set up.")
        print()

    if problems:
        print("  ** %d thing(s) to fix before running end of day. **" % problems)
        print()
        return 1

    print("  All clear.")
    print()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as e:  # network, login, config - say it plainly
        print("\n  Could not complete the check: %s\n" % e)
        sys.exit(1)
