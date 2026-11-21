import { test } from "node:test";
import assert from "node:assert/strict";
import { deltaT, weatherWarnings } from "./weather";
import { choiceLabels, prefill, timeText } from "./prefill";
import { emptyRecord, toAttributes, toRecord } from "../data/dailySchema";

const limits = { deltaTLow: 2, deltaTHigh: 8, deltaTStop: 10, windMinKmh: 3, windMaxKmh: 20 };

test("Delta T matches the worked example in survey123_spray_form_changes.md", () => {
  assert.equal(deltaT(18.2, 69), 3.7);
});

test("Delta T for the 20 Nov EP-C489.2 readings", () => {
  assert.equal(deltaT(28.5, 21), 13.4);
  assert.equal(deltaT(33.35, 17), 16.1);
});

test("Delta T is blank without both readings", () => {
  assert.equal(deltaT(null, 40), null);
  assert.equal(deltaT(25, null), null);
});

test("weather warnings", () => {
  assert.equal(weatherWarnings("Start", { temp: 18.2, rh: 69, wind: 10 }, limits).length, 0);
  const hot = weatherWarnings("Start", { temp: 28.5, rh: 21, wind: 34 }, limits);
  assert.equal(hot.length, 2);
  assert.match(hot[0], /should not go ahead/);
  assert.match(hot[1], /34 km\/h/);
});

test("Form 1 choice names become labels, spelling fixed", () => {
  assert.deepEqual(choiceLabels("jayme_peltz,calvin_mackereth"), ["Jayme Peltz (0429844652)", "Calvin Mackereth (0493342879)"]);
  assert.deepEqual(choiceLabels("llontrel, sp700_surfacant"), ["Lontrel", "SP700 Surfactant"]);
  assert.deepEqual(choiceLabels(""), []);
});

test("Survey123 times", () => {
  assert.equal(timeText("12:10:00.000", "Australia/Adelaide"), "12:10");
  assert.equal(timeText("7:05", "Australia/Adelaide"), "07:05");
  assert.equal(timeText(Date.parse("2026-11-20T05:30:00Z"), "Australia/Adelaide"), "16:00");
});

test("prefill from Form 1 beats job defaults; defaults used when Form 1 is blank", () => {
  const job = { job_number: "EP-C489.2", default_equipment: "Backpack Sprayer", default_chemicals: "Glyphosate 450; Pine oil" } as any;
  const r = prefill("EP-C489.2", "2026-11-20", "Australia/Adelaide", {
    start_time: "12:10", end_time: "16:00", operator_name: "jayme_peltz", chemicals: "glyphosate,brushwet,pine_oil",
    total_volume: 25, temperature: 28.5, humidity: 21, wind_speed: 34, wind_dir_text: "WNW", Comments: "Iron Knob Rd",
  }, job);
  assert.equal(r.operators, "Jayme Peltz (0429844652)");
  assert.equal(r.equipment, "Backpack Sprayer");
  assert.equal(r.chemicals, "Glyphosate 450; Brushwet/Consume; Pine oil");
  assert.equal(r.tank_total_l, 25);
  assert.equal(r.start_wind_dir, "WNW");
  const none = prefill("EP-C489.2", "2026-11-21", "Australia/Adelaide", null, job);
  assert.equal(none.chemicals, "Glyphosate 450; Pine oil");
  assert.equal(none.status, "draft");
});

test("record round-trips through attributes, text trimmed to field length", () => {
  const r = emptyRecord("DEW-NP", "2026-11-21");
  r.start_temp_c = 21.5;
  r.start_time = "08:30:00"; // longer than the 5-character field
  const back = toRecord(toAttributes(r) as any, 7);
  assert.equal(back.objectId, 7);
  assert.equal(back.start_temp_c, 21.5);
  assert.equal(back.start_time, "08:30");
  assert.equal(back.end_temp_c, null);
});
