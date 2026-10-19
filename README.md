# MSA EcoSync

Spatial logging platform for weed control operations. Records spray treatments
as points and polygons on an ESP32, stores them offline on SD, and syncs to
ArcGIS Online hosted feature layers.

Built by Mapping Services Australia.

---

## The unit

**EcoSyncField** — trigger-mounted logger for vehicle, backpack and on-foot
spraying. Records automatically as the operator treats. Two units built, both
in service.

The direction of travel is a general-purpose field data logger rather than a
spray-only device: the four dashboard buttons already carry configurable
target, control type and capture mode, so pest observations, infrastructure and
survey points are the same mechanism with different labels. The ArcGIS schema
is the constraint, not the firmware — see **Open questions** below.

*Previously explored and not currently pursued:* EcoSync Micro (wearable
variant — superseded by making Field itself untethered) and EcoSync Ute
(vehicle hub, `docs/ecosync_ute_concept.md`).

**EcoSyncID** — AI weed detection from vehicle imagery. In development,
separate repository. Spec and design targets in `docs/`.

---

## Hardware

| | |
|---|---|
| Board | SparkFun Thing Plus ESP32 WROOM, FQBN `esp32:esp32:esp32thing_plus_c` |
| GNSS | u-blox **DAN-F10N**, integrated L1/L5 patch antenna |
| Firmware reported | `FWVER=SPGL1L5 6.00`, `PROTVER=40.00` |
| Constellations | GPS, Galileo, BeiDou, QZSS, NavIC, SBAS |
| Augmentation | **SouthPAN L1 SBAS**, free, no subscription |
| Battery | 3.7 V 2000 mAh LiPo with fuel gauge |
| Storage | microSD |

**No power switch.** The unit runs continuously and must be charged nightly.
Measured draw is 2.8 %/h, so roughly 28 hours per charge. A Pololu 2808
pushbutton power switch is the planned fix; see **Current work**.

---

## Build and flash

```
tools\build.bat            compile only        -> build_log.txt
tools\flash.bat COM3       compile and upload  -> flash_log.txt
tools\flash_rawx.bat       flash the RAWX test sketch (replaces field firmware)
```

Requires `arduino-cli` on PATH and the ESP32 core. `build.bat` installs the
core and the two pinned libraries if they are missing.

**After every flash, check the build stamp.** The serial monitor prints
`Firmware build: ...` at 115200, and the same string shows on the dashboard
under the title. If it does not match what you just built, you are testing the
wrong binary — this has cost real time before.

Current field build: **`20261003-a uart buffer`**.

---

## Layout

```
firmware/ecosync_field/   the live sketch. Folder and file share a name so
                          arduino-cli takes it directly, with no copy step.
firmware/rawx_test/       standalone raw-measurement test (see GNSS below)
firmware/archive/         superseded builds, kept for reference
hardware/                 OpenSCAD enclosures, mounting, BOM notes
docs/                     GNSS decisions, StoryMap copy, field results, reviews
tools/                    build, flash, and the analysis scripts below
config.example.txt        every SD-card setting, with all values blank
```

**Tools**

| Script | What it does |
|---|---|
| `spray_stats.py` | positional data quality for a given day; optional web map extent |
| `track_to_polygons.py` | builds treated-area polygons from track points, keyed on unit + run |
| `get_ca_pem.ps1` / `.bat` | fetches the TLS root certificate for the SD card |
| `redact_config.ps1` / `.bat` | masks secrets in `config.txt` so it can be shared safely |

---

## Configuration

Nothing sensitive is compiled in. Every setting lives in `/config.txt` on the
SD card, and most can be changed from the unit's web dashboard.

Copy `config.example.txt` to the SD card as `config.txt` and fill it in, or set
the values through the dashboard and let the unit write the file itself.

`device_id` is the unit's identity. It is written to `job_id` on every record
and is how two units' work is told apart in ArcGIS. **Changing it on a unit
that has already uploaded splits that unit's history across two names.**

The dashboard splits by role: operators get target species, control methods and
job number; accuracy thresholds, capture geometry, network credentials and the
API key sit behind an administrator password.

**Forgot the admin password?** Hold the spray trigger down while powering on.
The unit clears it and says so on the serial console.

### Button slots

Four slots, each with `target<N>`, `control<N>`, `mode<N>` and two tuning
numbers. Mode is `POINT`, `CONTINUOUS` or `POLYGON`, and decides what the
tuning numbers mean. For `POINT`, `tunea` is the cooldown in seconds and
`tuneb` is the movement in metres that releases it early.

**Point de-duplication matters more than it looks.** A press logs when *either*
the cooldown has passed *or* the operator has moved far enough. With `tuneb=0`
the distance rule is disabled and the only separator is time, which fragments
one long spray into many records. Spatial analysis of 14 October 2026 put
nearest-neighbour plant spacing at a 5th percentile of 3.2 m — so **3.0 m is
the right distance value**, and 4 m starts merging genuinely separate plants.

---

## Data on the card

| File | What it is |
|---|---|
| `pending.csv` | points waiting to upload — **a queue, not an archive** |
| `pending_poly.csv` | polygons waiting |
| `pending_track.csv` | track waiting |
| `poly_journal.txt` | crash-safe polygon recovery |
| `rejected.csv` | records that failed validation — persists, forensic value |
| `bootlog.csv` | boot count, reset reason, firmware build |
| `config.txt`, `ca.pem` | configuration and TLS root certificate |

Records are removed from `pending.csv` once they upload. **The unit keeps no
local copy of synced records.** If an upload goes wrong there is nothing on the
card to reconcile against — see **Open questions**.

---

## NMEA position feed

With `nmea_out=1`, the unit serves NMEA 0183 over TCP on port **10110**
(`nmea_port`). Phone apps can then draw position from the unit instead of the
handset.

In **ArcGIS QuickCapture** or **Field Maps**: Settings → Location Provider →
Add Provider → **Network connection**, then the unit's address and port.
`192.168.4.1` on the unit's own access point, or its LAN address when both are
on the same Wi-Fi — the dashboard displays whichever applies.

This works on iPhone. Apple's MFi restriction applies only to *Bluetooth*
receivers, not network providers. The board keeps its existing QuickCapture
projects and forms, and only the position source changes.

GST is enabled, so apps receive and record real horizontal accuracy rather than
estimating it.

---

## Field results

**Head-to-head against an iPhone** — same position, same moment, 36 paired
readings over 3 field days:

| | EcoSync | iPhone |
|---|---|---|
| Average accuracy | **2.2 m** | 5.2 m |
| Best single reading | **0.98 m** | 2.80 m |
| EcoSync more accurate | **35 of 36 readings** | — |

**Operational record** — best day, 5 October 2026: 1,450 records, 1.7 m
average, 2.8 m worst, 100% within 3 m.

Across five field days the layer holds roughly 3,100 records, **but that figure
mixes EcoSync device records with Survey123 cut-and-swab entries written to the
same layer.** The device-only total has not yet been separated. Filter on
`capture_mode`, which only the device populates.

Every record stores its own `gps_accuracy_m`. Lead with the comparison and the
per-record auditability, never with a single headline figure.

**Do not use 14 October 2026 as a performance figure.** That day's 2.6 m mean
and 70% within 3 m were produced by a unit with the power fault described
below, restarting constantly and never letting the receiver settle.

---

## GNSS — what is and is not possible on this hardware

**SouthPAN L1 SBAS is in use and working.** Geoscience Australia commits to
≤3 m horizontal (95%); the unit measures 1.5–1.7 m, roughly double its own
spec. No subscription, nothing to lose when the network drops.

**RTK is not possible.** The DAN-F10N accepts UBX and NMEA only. There is no
RTCM input path, so an NTRIP stream cannot reach it.

**PPK is not possible either — tested, not assumed.** `firmware/rawx_test/`
enables `RXM-RAWX` and `RXM-SFRBX` on the RAM layer and listens. Result:

```
RXM-RAWX on UART1   key 0x209102A5 -> NAK (key not supported)
RXM-SFRBX on UART1  key 0x20910232 -> ACK
120 s listening: 0 RAWX frames, 0 measurements, 0 bad checksums
```

Definitive, not inconclusive: MON-VER answered in full and two of three config
keys were accepted, so the link, the protocol and the configuration interface
all demonstrably work. The module understood the request and refused that key.
Raw output is gated to the `UBX-F10050-KB-RW` chip variant, which this is not.

Both would need different hardware — an F9 or X20-class module, with an
external antenna. See `docs/` for the full write-up and the SouthPAN roadmap
through to 2028.

**Two firmware items that will go stale:** `configureSouthPAN()` hardcodes a
PRN 122 search mask, and `CFG-SBAS-USE_TESTMODE` is enabled. Both need
re-checking as SouthPAN moves toward certification.

---

## Known issues

**EcoSync 002 — intermittent power fault.** `bootlog.csv` shows 489 boots
against 34 logged lines: roughly 455 resets too fast for the unit to finish an
SD write. All reset reason 1, power-on. This is the root cause of most of what
was seen on 14 October 2026:

- **Config loss.** `saveConfigToSD()` deletes `config.txt` before rewriting it.
  A power cut inside that window leaves no file, and the next boot writes
  defaults over the card. Wi-Fi and AP passwords, device id and all tuning are
  lost silently.
- **Spurious records.** `hasLoggedPoint` lives in RAM, so the first trigger
  edge after any reset logs unconditionally — no cooldown, no distance check.
  87 of 269 records that day arrived in bursts 3–4 seconds apart (ESP32 boot
  time) with the unit stationary.
- **Degraded accuracy.** Every reset restarts the receiver cold and discards
  its converged solution and SBAS lock.

Suspect the power path — battery leads, JST connector, board power pads —
before the GNSS joints.

**No power switch.** Charge nightly. An unclean shutdown is the trigger for the
config loss above, and pulling the battery is currently the only way to turn a
unit off.

---

## Current work

- Repair 002's power path, then bench-soak watching the boot counter
- Atomic config save: write `config.tmp`, rename, keep `config.bak`
- Boot-edge guard: require a released trigger before the first logged press
- Persist last-point state so a reset cannot bypass de-duplication
- Fit the Pololu 2808 power switch (GPIO33 → OFF, 10k pull-down) with a
  triple-press shutdown gesture and a stuck-trigger guard
- Mounting: Garmin quarter-turn base secured with bands, one interface on every
  sprayer, under test

---

## Open questions

**Schema.** Fields are `weed_species` and `control_type`. If the unit is to log
pest observations and general survey data, those names stop fitting. Either
generalise them, or run separate layers per use case with the config pointing
at the right one.

**No local archive.** `pending.csv` drops records once they upload, so a unit
carries no independent record of what it logged. An append-only `logbook.csv`
would cost almost nothing on a card this size and is the answer to "what if the
upload is wrong?"

**Service URLs are hardcoded** at firmware lines 105–107 rather than read from
`config.txt`. This has to move before units go to organisations with different
destinations.

---

## Credentials — read before committing anything

`config.txt` and `config_new_keys_*.txt` hold the live Wi-Fi password and the
ArcGIS API token. `tools/spray_stats_config.json` holds an ArcGIS account
password. All are in `.gitignore` and must stay there.

**Git history is permanent.** A token committed once and deleted in the next
commit is still in the history, still clonable, and readable by anyone who is
ever given access. Before your first push:

```bash
git status
git ls-files | grep -iE "config|pem|token"           # Git Bash
git log --all --oneline -- config.txt tools/spray_stats_config.json
```

```
git ls-files | findstr config                         :: CMD
```

The only config files that should appear are `config.example.txt` and
`tools/spray_stats_config.example.json`. If the `git log` line returns anything
at all, rotate the credentials — removing the file now does not help.

`redact_config.bat` produces a shareable copy with `wifi_pass`, `ap_pass`,
`esri_token` and `admin_pass` masked.

---

## Verification

**There is currently no test harness in this repository.** A host-side harness
was used earlier in development and is not committed here. Treat the firmware
as covered by field testing only until it is rebuilt.

Bugs that host-side testing caught, which field testing did not: a config
reader silently truncating at 40 lines, an unauthenticated save endpoint
applying admin settings, secrets echoed into page source, and a
whitespace-trimming mismatch that locked the admin page.

Bugs the field caught instead: the NMEA relay writing to the socket one byte at
a time and blocking the main loop, a trigger debounce that assumed `loop()` was
faster than a squeeze, a 256-byte UART RX buffer overflowing at 21 satellites,
and the config-loss path described under **Known issues**.

Rebuilding the harness is the highest-value item outstanding before units go to
other organisations. Every failure of 14 October 2026 was logic rather than
hardware — config handling on a failed read, de-duplication state not
surviving a reset, a cooldown value fragmenting one plant into nineteen
records — and all three are the kind a host-side test catches in a second.
Priority targets: config parse and round-trip, point de-duplication rules,
polygon geometry, the CSV pending/sync path, and the admin privilege boundary.
