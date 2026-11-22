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
