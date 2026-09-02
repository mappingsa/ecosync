# EcoSync Field — first field test

## Before leaving

1. **Run `build.bat` and confirm `RESULT: SUCCESS`.** The last log predates the `HEX` fix,
   so this build has not been verified yet.
2. Flash the board.
3. Put `config.txt` in the **root of the SD card**, with `wifi_ssid` and `wifi_pass` filled in.
   - Using the old card? Leave its existing `config.txt` — it migrates automatically on
     first boot and keeps your targets and job number. Then add the Wi-Fi lines via the
     config page.
4. No `/ca.pem` on the card means TLS is unverified. Fine for a test; the dashboard says so.

## The one new behaviour that will look like a fault

**Sprays are now refused when GPS accuracy is worse than 5 m.** This is deliberate — it is
what stops 0,0 records reaching ArcGIS — but if conditions are poor it can look like the
trigger has stopped working.

- The dashboard shows **LOGGING ENABLED** or **LOGGING BLOCKED** under the accuracy figure.
- A refused spray sets Last Action to `NOT LOGGED - <reason>`, e.g.
  `Accuracy 8.3m > 5.0m limit`.
- If it is too strict for the paddock, raise **Max GPS accuracy** on the config page.
  No reflash needed.

## Boot check (serial monitor, 115200)

Expect: `Config loaded from SD` · `Success: GPS Connected (auto-PVT enabled)` ·
`Wi-Fi connected: <ip>` · any recovered backlog.

If Wi-Fi fails the unit raises its own AP — join **MSA-EcoSync-Setup** (password
`msaecosync`) and browse to the dashboard to fix the credentials.

## Worth exercising

- **Points** — a few sprays, confirm the counter climbs.
- **Polygon** — drive a loop, confirm the vertex count climbs while moving and *stops*
  while parked. That is the distance gate working.
- **Disarm mid-polygon** — new behaviour: it should **save**, not discard. Last Action
  reads `Saved polygon, N pts (safety engaged)`.
- **Power-cycle with data pending** — counters must come back, not reset to zero.
- **Sync** — counters go to zero, Last Action reads `SUCCESS!`.

## After syncing, check in ArcGIS

- Polygons render as **filled shapes, not holes** (ring-orientation fix).
- Timestamps are correct — Adelaide is UTC+9:30, so check the offset applied is what
  you expect.
- **No duplicates.** Sync twice deliberately if you want to prove it.

## Bring back if anything is off

- The serial log (it names the reason for every refusal).
- `/rejected.csv` from the SD card — entries there mean records were quarantined rather
  than uploaded, and the first column says why.
- Any leftover `/pending.csv` or `/pending_poly.csv`.
