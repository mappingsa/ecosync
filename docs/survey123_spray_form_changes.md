# Spray Record Form — changes to make

For the Survey123 survey at `ArcGIS\My Survey Designs\Form 1`. Written 20 Sep 2026.

> **These changes have been applied.** `Form 1.xlsx`, `scripts/weather.js` and
> `Spray Report form template.docx` are all updated on disk. This note is the record of what
> changed and why — read it before republishing, and keep it for when you wonder later why the
> Delta T calculation looks the way it does.

Three things: a Delta T bug that was fixed first, the end-of-day weather reading, and positional
accuracy on the report.

---

## 1. Delta T is being calculated wrongly — fix this first

The current calculation on `delta_t` is:

```
round(${temperature} - ((${temperature} * 0.6) + ((${temperature} - ((100 - ${humidity}) div 5)) * 0.4)), 1)
```

Work the algebra through and the temperature terms cancel:

```
T − (0.6T + 0.4(T − (100−H)/5))
  = T − 0.6T − 0.4T + 0.08(100−H)
  = 0.08 × (100 − RH)
```

**Temperature has no effect on the result at all.** The number depends only on humidity.

At the conditions on the 30 August report — 18.2 °C, 69% RH:

| | Delta T |
|---|---|
| Current formula | **2.5** |
| Correct value (Stull wet-bulb) | **3.7** |
| Printed on that report | 3.8 |

Your report shows 3.8, which is close to correct — so an earlier version of the form was right and
this has been broken since. Worth checking any reports issued after the change.

**Why this matters beyond tidiness.** Delta T is the spray-drift decision variable — the working
window is roughly 2 to 8, and above 10 you should not be spraying. A figure that ignores
temperature will read the same at 10 °C as at 35 °C, and the error grows with heat:

| Temp (RH 69%) | Current | Correct |
|---|---|---|
| 10 °C | 2.5 | 3.1 |
| 20 °C | 2.5 | 3.9 |
| 30 °C | 2.5 | 4.6 |
| 35 °C | 2.5 | 4.9 |

It under-reports, which is the wrong direction to be wrong in — it makes conditions look safer than
they are, on a document that is a compliance record.

**The fix** — replace the `calculation` on `delta_t` with:

```
pulldata("@javascript", "weather.js", "getDeltaT", ${temperature}, ${humidity})
```

`getDeltaT` is in the updated `weather.js` and uses the Stull (2011) wet-bulb approximation, which
needs `atan()` — a function XLSForm does not have and JavaScript does. That is why it belongs in
the script rather than in a formula cell.

---

## 2. The end-of-day weather reading

The regulations want wind speed, wind direction and temperature at **start and finish**. You
currently capture one set.

### How the trigger works

`pulldata("@javascript", ...)` re-runs when one of its **arguments changes**. So the end-of-day
fetch has to take something you fill in at the end. `${end_time}` is ideal — you enter it anyway.

`getWeatherAt(lat, lon, trigger)` in the updated script ignores `trigger` entirely; its only job is
to create that dependency, and to return nothing until the finish time exists.

### Protect the start reading first

Change the `calculation` on the existing `weather_json` to wrap it in `once()`:

```
once(pulldata("@javascript", "weather.js", "getWeather", ${current_lat}, ${current_lon}))
```

**Without this, reopening the form in the afternoon can silently recalculate the morning reading
and replace it with current conditions.** You would not notice, and the record would be wrong.

`once()` keeps the first *non-empty* value. That is why the updated `getWeather` returns `""`
rather than a message when it cannot reach the API — an empty return lets it try again, where a
"NETWORK ERROR" string would be frozen in permanently.

### Rows to add to the `survey` sheet

Put these straight after the existing `delta_t` row.

| type | name | label | calculation | appearance | bind::esri:fieldType | bind::esri:parameters |
|---|---|---|---|---|---|---|
| `calculate` | `end_weather_json` | | `pulldata("@javascript", "weather.js", "getWeatherAt", ${current_lat}, ${current_lon}, ${end_time})` | | `null` | |
| `decimal` | `end_temperature` | Air Temperature at finish (°C) | `pulldata("@json", ${end_weather_json}, "main.temp")` | `calculator` | | `calculationMode=whenEmpty` |
| `decimal` | `end_humidity` | Relative Humidity at finish (%) | `pulldata("@json", ${end_weather_json}, "main.humidity")` | `calculator` | | `calculationMode=whenEmpty` |
| `decimal` | `end_wind_speed` | Wind Speed at finish (km/h) | `round(pulldata("@json", ${end_weather_json}, "wind.speed") * 3.6, 1)` | `calculator` | | `calculationMode=whenEmpty` |
| `calculate` | `end_wind_dir` | | `pulldata("@json", ${end_weather_json}, "wind.deg")` | | | |
| `text` | `end_wind_dir_text` | Wind Direction at finish | `pulldata("@javascript", "weather.js", "getWindDirection", ${end_wind_dir})` | | | `calculationMode=whenEmpty` |
| `decimal` | `end_delta_t` | Delta T at finish | `pulldata("@javascript", "weather.js", "getDeltaT", ${end_temperature}, ${end_humidity})` | `calculator` | | `calculationMode=whenEmpty` |

`calculationMode=whenEmpty` is doing real work here: the script fills the field, but **anything you
type by hand survives** and is never overwritten.

### Also worth doing

Relabel the existing four weather fields so the report is unambiguous:

| name | current label | change to |
|---|---|---|
| `temperature` | Air Temperature (°C) | Air Temperature at start (°C) |
| `humidity` | Relative Humidity (%) | Relative Humidity at start (%) |
| `wind_speed` | Wind Speed (km/h) | Wind Speed at start (km/h) |
| `wind_dir_text` | Wind Direction | Wind Direction at start |

Add `calculationMode=whenEmpty` to those four as well, so a handheld-meter reading you type in is
never replaced by the API.

**Do not rename the fields themselves.** The names are baked into the published feature layer and
the report template; changing them breaks both.

### The compliance point underneath all this

Your weather comes from **OpenWeatherMap**, which for rural South Australia is largely interpolated
model data rather than an observation from a station you could name. The regulation wants the
conditions *at the site*.

Making these fields editable — populated from the API, overwritable from your handheld meter —
changes the record from *"a weather service estimated"* to *"the operator observed"*. That is a
one-column change and it is the difference between a plausible record and a defensible one.

---

## 3. Positional accuracy on the report

Every EcoSync point and polygon already carries `gps_accuracy_m`, and polygons store the **worst**
value anywhere on the perimeter rather than an average. It appears nowhere in the output.

### What to show

Do **not** put it on the map — the map already colours by control method, which carries more
information. A block beside the weather block:

> **POSITIONAL DATA QUALITY**
> Records captured: 247 points, 12 polygons
> Average accuracy: 1.8 m
> Worst recorded: 3.4 m
> Within 3 m: 96%
> Receiver: u-blox F10 dual-band L1/L5, SouthPAN augmented
>
> *Every individual record carries its own accuracy value in the layer.*

That last line is what makes it evidence rather than a claim — it says the summary is auditable.

### Where the numbers come from

ArcGIS feature-layer statistics support count, sum, min, max, average and stddev — **not median**.
So use average and worst, and derive "within 3 m" from two filtered counts.

Build those as indicator elements on the MSA Reporting Dashboard filtered to the job, then add
plain fields to the survey and type them in. Rows for the `survey` sheet:

| type | name | label |
|---|---|---|
| `integer` | `acc_points` | Points captured |
| `integer` | `acc_polygons` | Polygons captured |
| `decimal` | `acc_avg` | Average accuracy (m) |
| `decimal` | `acc_worst` | Worst accuracy (m) |
| `integer` | `acc_within3` | Percent within 3 m |

Manual for now. This is exactly what the spray-record generator automates later — see
`platform_strategy.md`, rung 1.

---

## 4. Report template changes

`Spray Report form template.docx`. Replace the weather block with a start/finish table, since a
single row is the gap Rapid Logix's report does not have:

```
WEATHER VERIFICATION COMPLIANCE
------------------------------------------------------------------
                    At start            At finish
Air Temperature:    ${temperature}°C    ${end_temperature}°C
Relative Humidity:  ${humidity}%        ${end_humidity}%
Wind:               ${wind_speed} km/h from ${wind_dir_text}    ${end_wind_speed} km/h from ${end_wind_dir_text}
Delta T:            ${delta_t}          ${end_delta_t}
```

Add after the total-litres block:

```
POSITIONAL DATA QUALITY
------------------------------------------------------------------
Records captured:   ${acc_points} points, ${acc_polygons} polygons
Average accuracy:   ${acc_avg} m
Worst recorded:     ${acc_worst} m
Within 3 m:         ${acc_within3}%
Receiver:           u-blox F10 dual-band L1/L5, SouthPAN augmented

Every individual record carries its own accuracy value in the layer.
```

And at the foot, which Rapid Logix has and this does not — PIRSA's own record-keeping form carries
one:

```
This is an accurate record of chemical use.

Operator signature: ..............................   Date: ..............
```

### Two typos on the current template output

- "Vehicle Mounted **Foiler** Spray Rig" → **Foliar**
- "**Vigillant**" → **Vigilant**

Both come from choice-list entries in the `choices` sheet, not the template.

---

## 5. One security note

The OpenWeatherMap API key is hard-coded in `weather.js`, and the `scripts` folder is published
**with the survey**. Anyone who can download the survey has the key.

For a free-tier weather key that is a small exposure — but it is billable and rate-limited, and the
habit is the thing worth changing. Treat that key as public: do not reuse it anywhere else, and
rotate it if the survey is ever shared outside MSA.

There is no good secrets story for client-side JavaScript in Survey123. If it ever needs to be
genuinely protected, the call has to move server-side — which is another argument for the spray
record generator owning this rather than the form.

---

## Order I would do these in

1. **Delta T fix** — it is wrong on live records now.
2. **`once()` on `weather_json`** — stops the morning reading being silently overwritten.
3. **End-of-day rows** — closes the biggest compliance gap against Rapid Logix.
4. **Accuracy block** — the differentiator nobody else can print.
5. **Signature line and the two typos** — five minutes.
