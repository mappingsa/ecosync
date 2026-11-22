"""Pure helpers for the ArcGIS importer: matching free text to the domain
lists, and Esri JSON geometry to EWKT for PostGIS. No network, so these are
unit-tested (test_mapping.py)."""

import re


def norm(value):
    """Job numbers and names are typed by hand: ignore case and stray spaces."""
    return re.sub(r"\s+", " ", str(value or "")).strip().upper()


def attr(attrs, name):
    """Case-insensitive attribute lookup (Job_Number vs job_number)."""
    if name in attrs:
        return attrs[name]
    low = name.lower()
    for k, v in attrs.items():
        if k.lower() == low:
            return v
    return None


def build_index(rows, name_key, aliases_key=None):
    """{normalised name or alias: row id} for a domain list."""
    index = {}
    for r in rows:
        names = [r.get(name_key)] + list(r.get(aliases_key) or [] if aliases_key else [])
        for n in names:
            if n and norm(n) not in index:
                index[norm(n)] = r["id"]
    return index


def match(value, index):
    """Row id for a typed value, or None (blank or unknown)."""
    key = norm(value)
    return index.get(key) if key else None


# --- Esri JSON -> EWKT ------------------------------------------------------------

def _pt(p):
    return "%r %r" % (float(p[0]), float(p[1]))


def _clockwise(ring):
    """Clockwise with y up = an Esri outer ring; anticlockwise = a hole."""
    s = 0.0
    for (x1, y1), (x2, y2) in zip(ring, ring[1:]):
        s += (x2 - x1) * (y2 + y1)
    return s > 0


def esri_to_ewkt(g):
    """Esri JSON geometry (outSR 4326) -> EWKT; None if empty or unusable.
    Lines always become MULTILINESTRING and polygons MULTIPOLYGON, matching
    the column types."""
    if not g:
        return None
    if g.get("x") is not None and g.get("y") is not None:
        return "SRID=4326;POINT(%s)" % _pt((g["x"], g["y"]))
    if g.get("paths"):
        parts = [p for p in g["paths"] if len(p) >= 2]
        if not parts:
            return None
        return "SRID=4326;MULTILINESTRING(%s)" % ",".join(
            "(%s)" % ",".join(_pt(p) for p in part) for part in parts)
    if g.get("rings"):
        polys = []
        for ring in g["rings"]:
            if len(ring) < 4:
                continue
            ring = [tuple(p[:2]) for p in ring]
            if ring[0] != ring[-1]:
                ring.append(ring[0])
            if _clockwise(ring) or not polys:
                polys.append([ring])
            else:
                polys[-1].append(ring)  # hole belongs to the last outer ring
        if not polys:
            return None
        return "SRID=4326;MULTIPOLYGON(%s)" % ",".join(
            "(%s)" % ",".join("(%s)" % ",".join(_pt(p) for p in ring) for ring in poly)
            for poly in polys)
    return None


def track_runs(features):
    """Group track points into one line per (unit, run_id), in time order.
    Returns [{"key", "unit", "run_id", "job", "start", "end", "ewkt"}]; runs
    with fewer than two points are skipped (no line to draw)."""
    runs = {}
    for f in features:
        a = f.get("attributes") or {}
        g = f.get("geometry") or {}
        if g.get("x") is None or g.get("y") is None:
            continue
        rid = attr(a, "run_id")
        if rid in (None, ""):
            continue
        unit = str(attr(a, "job_id") or "")
        key = "%s:%s" % (unit, rid)
        runs.setdefault(key, []).append((attr(a, "treatment_time") or 0, g["x"], g["y"], a))
    out = []
    for key, pts in runs.items():
        pts.sort(key=lambda p: p[0])
        if len(pts) < 2:
            continue
        out.append({
            "key": key,
            "unit": key.split(":", 1)[0],
            "run_id": str(key.split(":", 1)[1]),
            "job": attr(pts[0][3], "Job_Number"),
            "start": pts[0][0],
            "end": pts[-1][0],
            "ewkt": "SRID=4326;MULTILINESTRING((%s))" % ",".join(_pt((x, y)) for _t, x, y, _a in pts),
        })
    return out
