# EcoSync as an external GNSS receiver for phone apps

*(Applies to both the Field unit — in service now — and the Micro.)*

Researched 26 Aug 2026, **confirmed working and updated 11 Sep 2026**. The idea is viable, it is
now demonstrated on hardware, and it sidesteps the biggest commercial obstacle in the whole
project.

---

## The MFi problem, and why it does not apply

On iOS, a Bluetooth GNSS receiver must be part of Apple's **MFi programme** to feed location to
apps. That is a formal Apple licensing process requiring an authentication coprocessor in the
hardware. It is why Bad Elf, Eos Arrow and Trimble R1 cost what they do, and it would be a
serious barrier to a small Australian manufacturer.

**QuickCapture does not require it**, because Bluetooth is not the only path. Esri's
documentation lists three ways to attach an external receiver:

1. Bluetooth — **MFi required on iOS**
2. **Network connection — host name and port** — no MFi
3. NMEA log file playback

SparkFun document the same route for their own RTK line: ArcGIS QuickCapture, Survey123 and
QField all connect over **TCP on Wi-Fi**. Only **Field Maps** insists on MFi.

The requirement in every case is simply **NMEA 0183 output**, which the u-blox module produces
natively.

**Confirmed 26 Aug 2026:** both QuickCapture and Survey123 connected over TCP to the Field unit.
No MFi, no Bluetooth, no licensing.

---

## Two architectures

### Option A — the unit runs its own access point (recommended)

The iPhone joins the unit's Wi-Fi. QuickCapture points at its IP and port.

- No hotspot configuration for the operator
- **Avoids the 5 GHz trap** described below entirely
- The phone keeps its cellular data for anything needing internet

Watch for iOS prompting about a Wi-Fi network with no internet access — the operator taps to
stay connected.

*Power note:* a permanently-on access point is the single largest current draw in the device —
roughly 45 mA, and it prevents the radio sleeping at all. Leaving `ap_always` on halves battery
life. Run the AP when a phone is actually attached, not as a standing default.

### Option B — the unit joins the iPhone's hotspot

Works, but SparkFun flag a real trap: **the iOS hotspot defaults to 5 GHz, and the ESP32 is
2.4 GHz only.** The operator must enable **Maximize Compatibility** in the iPhone's hotspot
settings to force 2.4 GHz, or the unit simply cannot see it.

This is the same 2.4 GHz constraint that caused the Starlink connection problem, and it will
catch people repeatedly. Prefer Option A.

---

## Firmware — built and in service

1. **NMEA output enabled** on the GNSS module alongside the UBX stream the logger uses.
2. **A `WiFiServer` on a configurable port**, relaying NMEA sentences as they arrive.
3. **Config entries** for enable/disable and port number, on the Advanced page.
4. **Connection details shown on the dashboard** — IP and port — so the operator can type them
   into QuickCapture without guesswork.

Logging continues normally while a phone is attached. The two functions are independent: the
unit keeps writing its own records to SD while simultaneously serving position to the phone.

**Sentences emitted:** `RMC VTG GGA GSA GSV GLL GST`. **GST is present**, so the accuracy
estimate reaches the app — which is what evidences the claim in an audit.

> **Two bugs worth remembering, both found in the field rather than by testing.**
>
> **GST silently failed to enable.** The library's `enableNMEAMessage()` returns a bool that was
> being discarded, and `saveConfigSelective(IOPORT)` never persisted message rates. The request
> is now retried, its result recorded and displayed, and `MSGCONF` is included in the save so it
> survives a power cycle.
>
> **The relay froze the unit.** It was writing to the socket **one byte at a time**, and
> `NetworkClient::write()` runs `select()` with a one-second timeout retried ten times — inside
> the GNSS parser, inside `loop()`. One sentence could stall the main loop for a minute. It now
> writes one sentence per call behind a non-blocking writability probe, and drops rather than
> blocks. Both have regression tests.

---

## Why this matters commercially — the part worth thinking about

**It removes the schema-alignment problem from the critical path.**

The hard question hanging over the Landscape Boards pitch is matching DEW's QuickCapture schema
so EcoSync data can flow into their system. That is real work, it depends on DEW handing over
their field definitions, and it is outside your control.

In this mode, none of that applies. **The board keeps using DEW's existing QuickCapture project,
unchanged.** Same fields, same domains, same workflow, same submission path. The only thing that
changes is that the position attached to each record improves from 3–8 metres to about a metre.

That reframes the offer substantially:

- **Before:** "Adopt our device and our data pipeline, and we will work out how to make it fit
  your database."
- **After:** "Keep everything you already do. Clip this to your belt and your existing records
  get three times more accurate."

The second is a far easier thing for a government body to say yes to. No procurement of a new
data system, no IT integration project, no retraining, no risk to an existing dataset. It also
makes the Micro useful to boards with no interest in the spray-logging side at all, which widens
the market well beyond the current pitch.

And it does not cannibalise the rest — the unit still logs independently to its own SD card and
syncs to your layers. Serving the phone is additive.

---

## MEASURED FIELD RESULTS — three days, 36 paired readings

This is the strongest evidence in the project, because it is measured in the actual operating
environment against the actual thing being replaced.

| | 26 Aug | 10 Sep | 11 Sep |
|---|---|---|---|
| Receiver | M9 (L1 only) | F10 (L1/L5) | F10 (L1/L5) |
| EcoSync mean | 2.82 m | 1.88 m | 1.77 m |
| iPhone mean | 5.47 m | 5.24 m | 4.96 m |
| EcoSync wins | 11 / 12 | 12 / 12 | 12 / 12 |

**Pooled, n = 36: EcoSync 2.16 m vs iPhone 5.22 m — 35 wins from 36.**
**Current hardware only (F10, n = 24): 1.82 m vs 5.10 m — 24 from 24.**

### The gap widens exactly where the work happens

Split by canopy, 11 Sep:

| Canopy | EcoSync | iPhone | Gap |
|---|---|---|---|
| Open sky | 1.37 m | 2.80 m | 1.43 m |
| Light | 1.44 m | 5.00 m | 3.56 m |
| Medium | 2.02 m | 5.55 m | 3.52 m |
| Heavy | 2.16 m | 5.70 m | 3.54 m |

**Open sky to heavy canopy, the phone degrades by 2.90 m. EcoSync degrades by 0.79 m — it holds
up 3.7× better.**

In the open a handset does reasonably well, so the margin is modest. Under canopy the phone's
small, poorly-sited antenna and its reliance on fused Wi-Fi/cell/motion data fall apart, while a
dedicated multi-band receiver with a proper antenna holds up. Boxthorn in gullies and scrub is
precisely the canopy case.

**This is the line to lead with.** It answers the objection a board will actually raise — *we
already have phones* — better than any accuracy figure can. A phone under a boxthorn is a
different instrument than a phone in the depot car park.

### Stability, not just accuracy

The operator's own observation across both F10 days: *"I found the logger mostly stable whereas
the iPhone would jump around a lot."* The standard deviations bear it out — 0.47 against 1.37 on
11 Sep. A record that lands consistently within a metre is more useful for re-treatment tracking
than one that averages well but scatters.

### SouthPAN augmentation

| | 10 Sep | 11 Sep |
|---|---|---|
| SBAS on | 1.59 m (sd 0.37) | 1.65 m (sd 0.39) |
| SBAS off | 2.17 m (sd 0.66) | 2.01 m (sd 0.59) |
| Benefit | 0.57 m | 0.36 m |

Two independent days, same direction, near-identical spreads. Worth **0.36–0.57 m**, and it
roughly halves variability.

**It acquires slowly and then holds.** On 11 Sep the first four readings were unaugmented and
every reading afterward was augmented, *including two under heavy canopy* — while two of the four
*unaugmented* readings were also heavy canopy. The early failures were the warm-up period, not
the vegetation. The receiver has to download a full SBAS message set including the ionospheric
grid, which cycles over several minutes; once held, corrections carry validity windows that ride
through brief blockages.

**Operationally:** switch on in the open at the vehicle, and do not start logging until the
dashboard shows SBAS. Those four unaugmented readings were avoidable warm-up loss — a free
accuracy gain with no hardware and no cost.

### Battery — a full working day

Power configuration took the unit from **12.1 %/h to 6.4 %/h**, measured 8:30 to 16:00 with a
half-hour break: 100% down to 55% over seven running hours. That is roughly **14 hours** to a
sensible floor.

Better than it looks, because this was measured on the **F10**, which draws more than the M9 it
replaced — the configuration savings did not merely offset the new module, they beat it. For a
board running a crew, this is the difference between charging at lunch and charging overnight,
and battery logistics is exactly the kind of unglamorous thing that kills adoption.

### The caveat to raise before it is raised for you

These are **reported** accuracy figures — the receiver's estimate of its own error against the
handset's estimate of its own error — not surveyed truth.

Handset figures are known to be optimistic because the phone fuses non-GNSS sources into its
estimate, so the real gap is more likely wider than narrower. But a GIS analyst is precisely the
person who spots self-reported accuracy, and it is far better raised by you than from the floor.

**The fix is cheap:** occupy a known point repeatedly — a survey mark, or a feature identifiable
unambiguously on high-resolution imagery — and measure actual displacement and scatter. One
afternoon converts the dataset from *the device grading its own homework* to *we checked it
against a mark*.

---

## Suggested StoryMap copy — Section 2

> **Claim guidance, current as of 11 Sep 2026.** The fitted module is a **u-blox F10**
> (PROTVER 40.0): dual-band L1/L5, 1.0 m CEP with SBAS, SouthPAN named in the platform
> datasheet and confirmed working in the field.
>
> You **may** write: "approximately one metre", "dual-band L1/L5", "SouthPAN augmented",
> "metre-level".
>
> You **may not** write: **"sub-metre"** (1.0 m CEP is ~2.1 m at 95%, and a GIS reader will read
> it as 95%), **"survey-grade"** (implies centimetres), or **a specific part number** (makes the
> pitch hostage to stock — write "the u-blox F10 dual-band platform").
>
> *The earlier prohibition on "approximately one metre" applied to the M9 and is withdrawn.*

**Precision Positioning for the Apps You Already Use:** Both EcoSync units double as a
high-accuracy positioning source for the mobile workflows a board already runs. Operators
continue working in ArcGIS QuickCapture or Survey123 exactly as they do today — same project,
same fields, same submission path — while the app draws its position from the unit's dedicated
dual-band GNSS receiver instead of the handset's. Records land in the existing database at
approximately one metre, against the 3-to-8-metre drift typical of phone GPS.

Measured side by side against an iPhone across three field days and thirty-six paired readings,
the EcoSync unit was more accurate in thirty-five of them. **The advantage roughly doubles once
the operator is under canopy** — which is where weed control actually happens.

For vehicle crews, the EcoSync Field already performs this alongside its spray logging: a single
unit records the treatment automatically and simultaneously improves the accuracy of any
observation the operator captures by hand. For foot-based work, the EcoSync Micro brings the same
capability off-vehicle.

No new data system, no schema migration, no retraining. The workflow is untouched; only the
positional quality changes.

---

*Why the Field unit matters most here:* it is the unit in the field now, so this is demonstrable
rather than promised. It also means a board can trial the accuracy improvement against their own
existing project before committing to anything else in the platform.

---

## Getting the spray data to a board — do not settle for a data dump

The phone-feed above solves *observation* logging. The **spray data** — the points and polygons
the trigger generates automatically — is a separate question, and a periodic export is the
weakest of the options available.

It also directly contradicts the StoryMap, which currently promises **"Direct-to-Board Syncing:
cuts out the middleman. Boards gain immediate, sovereign control over their own field
intelligence, allowing for rapid localised response times."** A monthly file handover is the
middleman, reintroduced. It would also undercut Section 6's live dashboard argument, since a
dashboard fed by an occasional export is not a live command centre.

Four options, worst to best for this pitch:

**1. Periodic export (CSV / File Geodatabase).** Simple, no integration. But it is manual, stale
by definition, kills the live-dashboard claim, and puts a person back in the chain of custody the
pitch attacks.

**2. Direct writes into the board's own layer.** What the schema-matching work was for. Needs
their field definitions, credentials, and — if DEW is on Enterprise behind a firewall — network
access a field unit may simply not have. Highest effort, highest fragility.

**3. Scheduled ETL from your layer into theirs.** MSA keeps its own hosted layers as now; a
scheduled job maps and copies into the board's. Schema mapping is done once, server-side, under
your control. Robust, but still a copy with a lag, and it is infrastructure you must run and
maintain.

**4. Share a hosted feature layer view — the one to lead with.** In ArcGIS Online you create a
**view layer** over your existing spray layers, filtered to that board's job numbers or region,
and share it with their organisation or a group. They add it to their own maps and dashboards and
see it live. No copy, no lag, no ETL to maintain, no schema migration on their side, and **you
retain ownership of the underlying data** — which matters commercially when the contract comes up
for renewal.

**Recommendation:** lead with option 4, keep option 3 in reserve for a board that insists records
must physically reside in their own database, and treat option 1 purely as an ad-hoc convenience —
never as the architecture.

Worth confirming: whether their ArcGIS Online organisation can consume a view layer shared from
outside their org, or whether policy requires data to sit inside their own Enterprise portal. That
single question decides between option 4 and option 3, and it is a good thing to ask DEW early —
a five-minute answer for them, and it shapes everything downstream.

### Accuracy now travels with every record

Both the point and polygon layers carry **`gps_accuracy_m`**. Points record the reading at
capture; polygons record the **worst** accuracy observed anywhere on the perimeter, not the mean,
so the figure cannot flatter a boundary that drifted partway round.

This matters more than it first appears. It means every record is self-evidencing: a board can
filter, audit or exclude on positional quality rather than taking the whole dataset on trust. It
is also a field DEW does not currently collect, so it should be introduced as something the
platform *adds* to their schema rather than something it requires of them.

---

## To verify before promising it

- ~~Confirm the Network connection option appears and accepts the unit.~~ **DONE 26 Aug 2026** —
  both **QuickCapture and Survey123** connected over TCP. No MFi, no Bluetooth.
- ~~Confirm which **NMEA sentences** are flowing.~~ **DONE 26 Aug 2026** — `RMC VTG GGA GSA GSV
  GLL GST`, with **GST present**.
- ~~Confirm the unit reaches metre-level with SouthPAN.~~ **DONE 10–11 Sep 2026** — corrections
  confirmed applied via `getDiffSoln()`, benefit replicated across two days.
- **Still to do: test against a real DEW QuickCapture project**, not just a generic one.
- **Still to do: a controlled test against a known mark**, to replace reported accuracy with
  measured displacement. This is the highest-value remaining item.
- Check whether the board's operators use **Field Maps** as well — that one *does* need MFi, and
  the answer there is "not supported", not a workaround.
- Confirm QuickCapture displays and records the **accuracy value** from the external receiver.

---

## Sources

- [ArcGIS QuickCapture — Use a high-accuracy receiver](https://doc.arcgis.com/en/quickcapture/help/highaccuracyuse.htm)
- [SparkFun RTK Everywhere — iOS connection methods](https://docs.sparkfun.com/SparkFun_RTK_Everywhere_Firmware/gis_software_ios/)
- [ArcGIS Field Maps — Prepare for high-accuracy data collection](https://doc.arcgis.com/en/field-maps/latest/prepare-maps/high-accuracy-data-collection.htm)
