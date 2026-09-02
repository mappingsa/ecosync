# MSA EcoSync Field — Production Readiness Review

**File reviewed:** `MSA_Weed_Tracker_copy_20260727162128.ino` (620 lines)
**Board:** SparkFun Thing Plus ESP32-WROOM
**Date:** 20 Aug 2026

---

## Blockers — fix before production

### 1. Partial sync creates duplicate records in ESRI
`syncStoredData()` reads `/pending.csv` line by line and POSTs each one. If any record fails, it `return`s **without deleting the file**. The next sync starts from line 1 again, so every record that already uploaded successfully gets uploaded a second time.

With a flaky Starlink/4G link this will happen regularly, and the duplicates land in your published feature layer where they're painful to clean up.

**Fix:** either
- write successfully-synced records out to a temp file and rewrite `pending.csv` with only the remainder, or
- batch the whole file into a single `addFeatures` call (ESRI accepts an array of features) so it's atomic — this also cuts N TLS handshakes down to 1, which is a large speed win.

Batching is the better option on both counts.

### 2. Points are logged with no GPS fix check
In the POINT branch of the trigger logic, `saveRecordLocally()` is called unconditionally. If there's no fix, `getLatitude()`/`getLongitude()` return 0 and you write a valid-looking record at **0°N 0°E** (Gulf of Guinea) into the CSV and then into ArcGIS.

The polygon background sampler *does* guard with `if (lat != 0.0 && lon != 0.0)`, but the polygon's **first** vertex (line 419-422) is unguarded too.

**Fix:** gate all logging on `live_gps_lock` (and ideally on `live_accuracy_m` being under a threshold, e.g. 5 m). Set `lastAction` to something visible like "REJECTED — No GPS Fix" so the operator knows the spray wasn't recorded.

### 3. Unsynced counters are RAM-only and lie after a reboot
`unsyncedPoints` / `unsyncedPolygons` start at 0 every boot. If the device power-cycles with pending records on the SD card, the dashboard reports `0 Pts / 0 Polys` while the data is still sitting there unsent. An operator will reasonably assume there's nothing to sync.

**Fix:** count the lines in `/pending.csv` and `/pending_poly.csv` during `setup()` and seed the counters from that.

### 4. Target and job names aren't escaped — malformed or broken uploads
The POST body is `application/x-www-form-urlencoded`, but `weedSpecies` and `jobNumber` are dropped in raw. These come from free-text fields in the config portal.

- A space (your defaults are literally `Target A`, `Target B`, `Target C`) is not valid in a form-encoded body — tolerated by most servers, but not guaranteed.
- An `&` in a target name terminates the `features` parameter and breaks the request entirely.
- A `+` silently becomes a space.
- A `"` or `\` breaks the JSON.

**Fix:** JSON-escape the string values, then URL-encode the entire POST body. A small `urlEncode()` helper plus a `jsonEscape()` helper covers it.

### 5. Credentials are hardcoded in the sketch
```c
const char* ssid = "Jimmys";
const char* password = "Punter2611";
```
This is in the source file, which means it's in every copy, every backup, and every version you send to anyone (including this one — worth rotating that password).

**Fix:** move Wi-Fi SSID/password into `/config.txt` on the SD card alongside the targets, and add fields for them in the config portal.

---

## High — will cause field problems

### 6. GPS polling timeout of 10 ms is almost certainly too short
```c
if (myGNSS.getPVT(10)) { ... }
```
With auto-PVT disabled, `getPVT(maxWait)` polls the module and waits `maxWait` for the reply. 10 ms is not enough time for a ~100-byte UBX packet over I²C in most conditions, so this will frequently return `false` and the dashboard will sit on "SEARCHING..." even with a good fix.

**Fix:** call `myGNSS.setAutoPVT(true)` in `setup()`. `getPVT()` then returns cached data immediately with no blocking at all — which is what the non-blocking design here was actually reaching for.

### 7. Battery I²C calls are inside the web handler
The comment at line 142 says "NO I2C CALLS HERE" — but eleven lines later `getBatteryPercentage()` and `lipo.getChangeRate()` both hit the I²C bus, inside the request handler, on a page that auto-refreshes every 2 seconds. That's the exact contention the comment was written to avoid, and it shares a bus with the GNSS module.

**Fix:** poll the fuel gauge on a timer in `loop()` (every 10–30 s is plenty) into globals, and read the globals in the handler like you already do for GPS.

Related: if `lipo.begin()` fails, `getSOC()` still gets called and returns garbage. Track a `batteryOk` flag and show `--%` instead.

### 8. Polygon rings may be interpreted as holes by ArcGIS
Esri polygon geometry requires **clockwise** vertex order for outer rings; counter-clockwise means a hole. Your ring is built straight from the GPS track, so whether a polygon renders correctly depends on which direction the operator happened to drive around the patch.

**Fix:** compute the signed area (shoelace formula) before building `ringsJson` and reverse the vertex order if it comes out counter-clockwise.

### 9. Unbounded String growth during long polygons
`currentPolygonCoords` grows by ~22 bytes every 2 seconds with no cap. A 1-hour polygon is ~40 KB of repeatedly-reallocated `String` on the ESP32 heap. Combined with the `String`-built HTML pages on every 2-second dashboard refresh, heap fragmentation over a full working day is a real crash risk.

**Fix:** cap vertex count (e.g. 2000, auto-close and start a new polygon), or `reserve()` a block up front, or stream vertices to a temp file on SD instead of holding them in RAM.

### 10. Disarming mid-polygon leaves the polygon open indefinitely
Both the timeout check (line 358) and the vertex sampler (line 372) are gated on `systemArmed`. Engage the safety mid-polygon and the polygon neither closes nor collects points — it just sits open until the system is re-armed, at which point it saves immediately with a stale timestamp and a gap in the track.

**Fix:** close and save the polygon in the `/toggleSafety` handler when disarming.

### 11. Switching mode silently discards an in-progress polygon
`/setMode` sets `polygonActive = false` with no save. Any partial polygon is lost with no warning.

**Fix:** save it if `vertexCount >= 3`, otherwise warn in `lastAction`.

---

## Medium — security and robustness

### 12. TLS certificate verification is disabled
`client.setInsecure()` means anyone who can present themselves as `services2.arcgis.com` gets your data. On a field device using unknown networks this matters.

**Fix:** pin the Esri root CA certificate with `client.setCACert()`.

### 13. Feature service appears to accept unauthenticated writes
There's no token in the `addFeatures` request, which implies the layer is publicly editable. If so, anyone who finds the service URL can write to your production weed layer.

**Fix:** if that's not deliberate, secure the layer and add OAuth token handling (generate via `generateToken` at sync time, cache until expiry).

### 14. Web interface has no authentication
Anything on the same network can arm the system, change targets and job numbers, trigger syncs, and rewrite the SD config. mDNS at `ecosyncfield.local` advertises it. Worth at least basic HTTP auth on `/config`, `/saveConfig` and `/toggleSafety`.

### 15. `loadTargetsFromSD()` doesn't validate what it reads
A truncated or short `config.txt` makes `readStringUntil('\n')` return empty strings, so targets become blank and the dashboard buttons become unlabelled. Only `currentJobNumber` has a fallback. Apply the same guard to all four fields.

### 16. No Wi-Fi reconnect in the main loop
`WiFi.mode(WIFI_STA)` is only set inside `syncStoredData()`, not before `WiFi.begin()` in `setup()`. There's no reconnect handling in `loop()`, so the dashboard becomes unreachable after any AP dropout until a sync is attempted.

### 17. `getUnixEpoch()` is unguarded
Called with the default (blocking) timeout, and returns 0 if the module has no time solution — which then becomes `"0000"` in the `treatment_time` field, i.e. 1 Jan 1970. Snapshot lat/lon/epoch into globals in the GPS poll block and use those cached values in the trigger handler; it removes both the blocking call and the zero-time problem.

---

## Low — tidy-up

- **18.** Section 8 header is spelled `ECOSUNC` (line 485).
- **19.** `currentJobID` is a hardcoded compile-time constant (`MSA_EcoSync_001`) sent as `job_id`, while `Job_Number` comes from the SD config. Two similarly-named fields with different sources is a reporting trap — confirm this is intentional.
- **20.** The dashboard `<meta http-equiv='refresh' content='2'>` reloads the entire page every 2 seconds. A small `fetch()` against a `/status` JSON endpoint would cut bandwidth and CPU substantially, and stop the page jumping while an operator is mid-tap.
- **21.** `delay(10)` at the end of `loop()` caps the trigger polling rate at 100 Hz. Fine for a pressure pad, just noting it interacts with the 100 ms debounce.

---

## Suggested order of work

1. Batch sync + atomic file handling (#1) — largest correctness win
2. GPS fix validation before logging (#2, #17)
3. `setAutoPVT` + cached GPS/battery globals (#6, #7)
4. String escaping for uploads (#4)
5. Credentials to SD config (#5)
6. Boot-time counter recovery (#3)
7. Ring orientation (#8)
8. Polygon lifecycle fixes (#10, #11) and RAM cap (#9)
9. Security hardening (#12, #13, #14)
