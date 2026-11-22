"""Run: python -m unittest test_mapping  (from the importer folder)."""

import unittest

from mapping import build_index, esri_to_ewkt, match, norm, track_runs


class Matching(unittest.TestCase):
    def setUp(self):
        self.jobs = build_index([
            {"id": "j1", "job_number": "DEW-NP", "aliases": ["DEW NP Weed Control"]},
            {"id": "j2", "job_number": "EP-C489.5", "aliases": ["Works Order C489.5"]},
        ], "job_number", "aliases")

    def test_number_and_alias(self):
        self.assertEqual(match("DEW-NP", self.jobs), "j1")
        self.assertEqual(match("dew np weed control ", self.jobs), "j1")
        self.assertEqual(match("Works  Order C489.5", self.jobs), "j2")

    def test_unknown_and_blank(self):
        self.assertIsNone(match("EP-C999", self.jobs))
        self.assertIsNone(match("", self.jobs))
        self.assertIsNone(match(None, self.jobs))

    def test_norm(self):
        self.assertEqual(norm("  African   boxthorn "), "AFRICAN BOXTHORN")


class Geometry(unittest.TestCase):
    def test_point(self):
        self.assertEqual(esri_to_ewkt({"x": 140.5, "y": -34.25}), "SRID=4326;POINT(140.5 -34.25)")

    def test_line(self):
        self.assertEqual(esri_to_ewkt({"paths": [[[1, 2], [3, 4]]]}),
                         "SRID=4326;MULTILINESTRING((1.0 2.0,3.0 4.0))")

    def test_polygon_with_hole(self):
        outer = [[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]                 # clockwise
        hole = [[0.4, 0.4], [0.6, 0.4], [0.6, 0.6], [0.4, 0.6], [0.4, 0.4]]  # anticlockwise
        w = esri_to_ewkt({"rings": [outer, hole]})
        self.assertTrue(w.startswith("SRID=4326;MULTIPOLYGON(((0.0 0.0,"))
        self.assertEqual(w.count("(("), 1)  # one polygon, two rings
        self.assertIn("),(0.4 0.4", w)

    def test_two_polygons(self):
        a = [[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]
        b = [[5, 5], [5, 6], [6, 6], [6, 5], [5, 5]]
        self.assertEqual(esri_to_ewkt({"rings": [a, b]}).count("(("), 2)

    def test_empty(self):
        self.assertIsNone(esri_to_ewkt(None))
        self.assertIsNone(esri_to_ewkt({"paths": [[[1, 2]]]}))


class Tracks(unittest.TestCase):
    def test_runs_grouped_by_unit_and_run_in_time_order(self):
        f = lambda unit, run, t, x: {"attributes": {"job_id": unit, "run_id": run, "treatment_time": t,
                                                    "Job_Number": "DEW-NP"}, "geometry": {"x": x, "y": -34}}
        runs = track_runs([f("u1", 100, 3, 3.0), f("u1", 100, 1, 1.0), f("u1", 100, 2, 2.0),
                           f("u2", 100, 1, 9.0), f("u2", 100, 2, 9.5), f("u1", 200, 1, 7.0)])
        by = {r["key"]: r for r in runs}
        self.assertEqual(set(by), {"u1:100", "u2:100"})      # single-point run u1:200 skipped
        self.assertEqual(by["u1:100"]["ewkt"], "SRID=4326;MULTILINESTRING((1.0 -34.0,2.0 -34.0,3.0 -34.0))")
        self.assertEqual((by["u1:100"]["start"], by["u1:100"]["end"]), (1, 3))


if __name__ == "__main__":
    unittest.main()


class Daily(unittest.TestCase):
    def test_row(self):
        from mapping import daily_row
        r = daily_row({"JOB_NUMBER": "EP-C489.5", "record_date": "2026-11-18", "start_time": "07:30",
                       "operators": "Jayme Peltz; Calvin ;", "tank_total_l": "400", "start_delta_t": "",
                       "entity": "fpe", "status": "Issued", "observations": ""})
        self.assertEqual(r["record_date"], "2026-11-18")
        self.assertEqual(r["operators"], ["Jayme Peltz", "Calvin"])
        self.assertEqual(r["equipment"], [])
        self.assertEqual(r["tank_total_l"], 400.0)
        self.assertIsNone(r["start_delta_t"])
        self.assertIsNone(r["observations"])
        self.assertEqual((r["letterhead"], r["status"]), ("FPE", "issued"))

    def test_bad_date_and_defaults(self):
        from mapping import daily_row
        self.assertIsNone(daily_row({"record_date": "18/11/2026"}))
        r = daily_row({"record_date": "2026-11-18", "entity": "", "status": ""})
        self.assertEqual((r["letterhead"], r["status"]), (None, "draft"))

    def test_kinds_and_names(self):
        from mapping import attachment_kind, safe_name
        self.assertEqual(attachment_kind("signature.png"), "signature")
        self.assertEqual(attachment_kind("Spray Record EP-C489.5 2026-11-18 v2.pdf"), "report")
        self.assertEqual(attachment_kind("photo_1.jpg"), "photo")
        self.assertEqual(safe_name("Spray Record EP-C489.5 2026-11-18 v2.pdf"), "Spray_Record_EP-C489.5_2026-11-18_v2.pdf")
