# MSA EcoSync

Spatial logging platform for weed control operations. Records spray treatments
as points and polygons on an ESP32, stores them offline on SD, and syncs to
ArcGIS Online hosted feature layers.

Built by Mapping Services Australia.

---

## The three units

| Unit | Status | What it is |
|---|---|---|
| **EcoSync Field** | In service | Trigger-mounted logger for vehicle and backpack spraying. Records automatically as the operator treats. |
| **EcoSync Micro** | In development | Wearable version for foot-based work — cut-and-swab, observations, revegetation. |
| **EcoSync Ute** | Concept | Vehicle hub: 12 V power, charging dock, Starlink backhaul so Field units sync from the paddock. **Not** a position source — see `docs/ecosync_ute_concept.md`. |

Both Field and Micro also serve NMEA over TCP, so ArcGIS QuickCapture and
Survey123 on a phone can draw position from the unit instead of the handset —
no MFi licensing, no change to the board's existing workflow.

---

## Layout

```
firmware/ecosync_field/   the live sketch. Folder and file share a name so
                          arduino-cli takes it directly, with no copy step.
firmware/archive/         superseded builds, kept for reference
hardware/                 OpenSCAD enclosures, mounting, BOM notes
docs/                     GNSS decisions, StoryMap copy, field results, reviews
tools/                    build.bat, flash.bat, repair_build.bat
config.example.txt        every SD-card setting, with all values blank
```

---

## Build and flash

```
tools\build.bat            compile only        -> build_log.txt
tools\flash.bat COM3       compile and upload  -> flash_log.txt
```

Requires `arduino-cli` on PATH and the ESP32 core. `build.bat` installs the
core and the two pinned libraries if they are missing.

Board: **SparkFun Thing Plus ESP32 WROOM**, FQBN `esp32:esp32:esp32thing_plus_c`.

**After every flash, check the build stamp.** The serial monitor prints
`Firmware build: ...` at 115200, and the same string shows on the dashboard
under the title. If it does not match what you just built, you are testing the
wrong binary — this has cost real time before.

---

## Configuration

Nothing sensitive is compiled in. Every setting lives in `/config.txt` on the
SD card, and most can be changed from the unit's web dashboard.

Copy `config.example.txt` to the SD card as `config.txt` and fill it in, or set
the values through the dashboard and let the unit write the file itself.

The dashboard splits by role: operators get target species, control methods and
job number; accuracy thresholds, capture geometry, network credentials and the
API key sit behind an administrator password.

**Forgot the admin password?** Hold the spray trigger down while powering on.
Held for three seconds, the unit clears it and says so on the serial console.

---

## Credentials — read before committing anything

`config.txt` and `config_new_keys_*.txt` hold the live Wi-Fi password and the
ArcGIS API token. They are in `.gitignore` and must stay there.

**Git history is permanent.** A token committed once and deleted in the next
commit is still in the history, still clonable, and readable by anyone who is
ever given access. Before your first push, run:

```
git status
git ls-files | findstr config
```

The only config file that should appear is `config.example.txt`.

---

## Field results, 26 Aug 2026

EcoSync Field against an iPhone, measured side by side, twelve paired readings:

| Conditions | EcoSync advantage |
|---|---|
| Clean air | 0.5 – 1 m better |
| Scrub, under canopy | **2.5 – 3 m better** |

Mean 2.82 m vs 5.47 m, and — the number that matters more — a standard
deviation of 0.38 m against the phone's 1.25 m. **The gap widens exactly where
the work happens.** Lead with the comparison, never with a single figure.

Current hardware is a u-blox **M9** (PROTVER 32.1): L1 only, 2.0 m CEP, and
SouthPAN is *not* in its supported SBAS list. Do not claim "approximately one
metre", "dual-band" or SouthPAN augmentation for the unit as it stands. Those
become true when the F10 module is fitted, not before. See
`docs/gnss_hardware_decision.md`.

---

## Verification

The firmware has a host-side test harness — currently 335 assertions covering
the sync path, config parsing, the privilege boundary, secret handling, polygon
geometry, point de-duplication, the NMEA relay and the trigger.

Bugs it has caught that field testing did not: a config reader silently
truncating at 40 lines, an unauthenticated save endpoint applying admin
settings, secrets echoed into page source, and a whitespace-trimming mismatch
that locked the admin page.

Bugs the field caught that it did not: the NMEA relay writing to the socket one
byte at a time and blocking the main loop, and a trigger debounce that assumed
`loop()` was faster than a squeeze. Both now have regression tests.
