"""Run: python -m unittest test_import_daily  - import_daily against fakes (no network)."""

import unittest
from unittest import mock

import import_arcgis as ia

ORG = "org1"
URL = "https://x/EcoSync_Daily_Records/FeatureServer/0"


class FakeSB:
    dry_run = False

    def __init__(self, existing_sheets=(), existing_atts=()):
        self.sheets = list(existing_sheets)
        self.atts = list(existing_atts)
        self.files, self.removed, self.deleted = {}, [], []

    def get(self, table, params):
        if table == "daily_records":
            return [r for r in self.sheets if r["source"] != ia.DAILY_SOURCE]
        return [a for a in self.atts if a.get("source_ref")]

    def upsert(self, table, rows, conflict, returning=None):
        if table == "daily_records":
            out = []
            for r in rows:
                r = dict(r, id="rec-%s-%s" % (r["job_id"], r["record_date"]))
                self.sheets.append(r)
                out.append({"id": r["id"], "job_id": r["job_id"], "record_date": r["record_date"]})
            return out
        self.atts.extend(rows)
        return []

    def put_file(self, path, data, ct):
        self.files[path] = data

    def remove_files(self, paths):
        self.removed.extend(paths)

    def delete(self, table, params):
        self.deleted.append(params["source_ref"][3:])


def feat(oid, job, date, **kw):
    a = {"objectid": oid, "globalid": "g%d" % oid, "job_number": job, "record_date": date}
    a.update(kw)
    return {"attributes": a}


FEATS = [
    feat(1, "EP-C489.5", "2026-11-18", status="issued", entity="FPE"),
    feat(2, "EP-C489.5", "2026-11-18", status="issued"),   # newer duplicate wins
    feat(3, "DEW NP Weed Control", "2026-11-20"),           # alias
    feat(4, "EP-C999", "2026-11-20"),                       # unknown job
    feat(5, "DEW-NP", "bad"),                               # bad date
    feat(6, "DEW-NP", "2026-11-21"),                        # saved in the new app already
]
ATTS = {"attachmentGroups": [
    {"parentObjectId": 2, "attachmentInfos": [
        {"id": 10, "name": "photo 1.jpg", "contentType": "image/jpeg", "size": 3},
        {"id": 11, "name": "signature.png", "contentType": "image/png", "size": 3},
        {"id": 12, "name": "Spray Record EP-C489.5 2026-11-18 v1.pdf", "contentType": "application/pdf", "size": 3}]},
    {"parentObjectId": 3, "attachmentInfos": []},
]}
JOBS = {"EP-C489.5": "jA", "DEW-NP": "jB", "DEW NP WEED CONTROL": "jB"}


def arc_post(url, token, params):
    if url.endswith("/sharing/rest/search"):
        return {"results": [{"title": "EcoSync Daily Records", "url": URL.rsplit("/0", 1)[0]}]}
    if url.endswith("/queryAttachments"):
        return ATTS
    raise AssertionError(url)


class Resp:
    content = b"abc"

    def raise_for_status(self):
        pass


class ImportDaily(unittest.TestCase):
    def run_import(self, sb):
        notes = []
        with mock.patch.object(ia, "arc_post", arc_post), \
             mock.patch.object(ia, "fetch_all", lambda u, t, w: FEATS), \
             mock.patch.object(ia.requests, "get", lambda *a, **k: Resp()), \
             mock.patch.object(ia, "log", lambda m: None):
            res = ia.import_daily(sb, ORG, "tok", JOBS, lambda s, f, v: notes.append(v))
        return res, notes

    def app_sheet(self):
        return {"job_id": "jB", "record_date": "2026-11-21", "source": "app", "id": "keep"}

    def test_first_run(self):
        sb = FakeSB([self.app_sheet()])
        (n, copied, removed, skipped), notes = self.run_import(sb)
        self.assertEqual((n, copied, removed, skipped), (2, 3, 0, 2))
        self.assertEqual(notes, ["EP-C999"])
        sheet = [r for r in sb.sheets if r.get("job_id") == "jA"][0]
        self.assertEqual((sheet["source_ref"], sheet["status"]), ("g2", "issued"))
        kinds = sorted(a["kind"] for a in sb.atts)
        self.assertEqual(kinds, ["photo", "report", "signature"])
        self.assertIn("org1/rec-jA-2026-11-18/12_Spray_Record_EP-C489.5_2026-11-18_v1.pdf", sb.files)

    def test_second_run_copies_nothing_and_removes_deleted(self):
        sb = FakeSB([self.app_sheet()])
        self.run_import(sb)
        sb.files.clear()
        ATTS["attachmentGroups"][0]["attachmentInfos"].pop(0)   # photo deleted in ArcGIS
        try:
            (n, copied, removed, _), _ = self.run_import(sb)
        finally:
            ATTS["attachmentGroups"][0]["attachmentInfos"].insert(0, {"id": 10, "name": "photo 1.jpg", "contentType": "image/jpeg", "size": 3})
        self.assertEqual((copied, removed), (0, 1))
        self.assertEqual(sb.files, {})
        self.assertEqual(sb.deleted, ["%s:2:10" % ia.DAILY_SOURCE])


if __name__ == "__main__":
    unittest.main()
