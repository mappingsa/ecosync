# EcoSync as an external GNSS receiver for phone apps

*(Applies to both the Field unit — shipping now — and the Micro.)*

Researched 26 Aug 2026. **The idea is viable, and it sidesteps the biggest commercial obstacle
in the whole project.**

---

## The MFi problem, and why it does not apply

On iOS, a Bluetooth GNSS receiver must be part of Apple's **MFi programme** to feed location
to apps. That is a formal Apple licensing process requiring an authentication coprocessor in
the hardware. It is why Bad Elf, Eos Arrow and Trimble R1 cost what they do, and it would be a
serious barrier to a small Australian manufacturer.

**QuickCapture does not require it**, because Bluetooth is not the only path. Esri's
documentation lists three ways to attach an external receiver:

1. Bluetooth — **MFi required on iOS**
2. **Network connection — host name and port** — no MFi
3. NMEA log file playback

SparkFun documents the same route for their own RTK line: ArcGIS QuickCapture, Survey123 and
QField all connect over **TCP on Wi-Fi**. Only **Field Maps** insists on MFi.

The requirement in every case is simply **NMEA 0183 output**, which the u-blox module produces
natively.

---

## Two architectures

### Option A — Micro runs its own access point (recommended)

The iPhone joins the Micro's Wi-Fi. QuickCapture points at the Micro's IP and port.

- No hotspot configuration for the operator
- **Avoids the 5 GHz trap** described below entirely
- The Micro already runs a permanent access point (`ap_always`), so most of this exists
- The phone keeps its cellular data for anything needing internet

Watch for iOS prompting about a Wi-Fi network with no internet access — the operator taps to
stay connected.

### Option B — Micro joins the iPhone's hotspot

Works, but SparkFun flag a real trap: **the iOS hotspot defaults to 5 GHz, and the ESP32 is
2.4 GHz only.** The operator must enable **Maximize Compatibility** in the iPhone's hotspot
settings to force 2.4 GHz, or the Micro simply cannot see it.

This is the same 2.4 GHz constraint that caused the Starlink connection problem, and it will
catch people repeatedly. Prefer Option A.

---

## Firmware work required

Modest, and it fits the existing architecture.

1. **Enable NMEA output on the GNSS module** alongside the UBX stream the logger already uses.
   The current code sets `COM_TYPE_UBX` only.
2. **Add a `WiFiServer` on a configurable port.** On a client connection, relay NMEA sentences
   straight through as they arrive. Little more than a byte pump — no parsing, no
   reformatting, since QuickCapture wants raw NMEA 0183.
3. **Config entries** for enable/disable and port number, on the Advanced page.
4. **Show the connection details on the dashboard** — IP and port — so the operator can type
   them into QuickCapture without guesswork.

Logging continues normally while a phone is attached. The two functions are independent: the
Micro keeps writing its own records to SD while simultaneously serving position to the phone.

---

## Why this matters commercially — the part worth thinking about

**It removes the schema-alignment problem from the critical path.**

The hard question hanging over the Landscape Boards pitch is matching DEW's QuickCapture
schema so EcoSync data can flow into their system. That is real work, it depends on DEW
handing over their field definitions, and it is outside your control.

In this mode, none of that applies. **The board keeps using DEW's existing QuickCapture
project, unchanged.** Same fields, same domains, same workflow, same submission path. The only
thing that changes is that the position attached to each record improves from 3–8 metres to
about a metre.

That reframes the offer substantially:

- **Before:** "Adopt our device and our data pipeline, and we will work out how to make it fit
  your database."
- **After:** "Keep everything you already do. Clip this to your belt and your existing records
  get five times more accurate."

The second is a far easier thing for a government body to say yes to. No procurement of a new
data system, no IT integration project, no retraining, no risk to an existing dataset. It also
makes the Micro useful to boards that have no interest in the spray-logging side at all, which
widens the market well beyond your current pitch.

And it does not cannibalise the rest — the Micro still logs independently to its own SD card
and syncs to your layers. Serving the phone is additive.

---

## Suggested StoryMap copy — new bullet for Section 2

> **CORRECTED 2026-08-26 — read this before pasting anything below.**
>
> The Field pilot's module was identified from its own diagnostics page: **PROTVER 32.1**,
> a **u-blox M9**. Per the
> [NEO-M9N-00B datasheet](https://content.u-blox.com/sites/default/files/NEO-M9N-00B_DataSheet_UBX-19014285.pdf)
> that means **L1 only**, **"2.0 m CEP"** (~4.2 m at 95%), and an SBAS list of
> **"EGNOS, GAGAN, MSAS and WAAS"** — **SouthPAN is not named**.
>
> So for the unit that exists today: **do not write "dual-band" or "L1/L5"**, **do not write
> "approximately one metre"**, and **do not imply SouthPAN augmentation**. Those become true
> when the F10 module is fitted, not before.
>
> **Never write "sub-metre"** in any version; see `gnss_hardware_decision.md`.
>
> The claim that IS demonstrated, today, on hardware in the field: phone GPS 3–8 m improved to
> **about two metres**, feeding QuickCapture and Survey123 live. That is a genuine 2–4×
> and it needs no inflating.

---

## MEASURED FIELD RESULT — 26 Aug 2026, EcoSync Field vs iPhone, side by side

This is the strongest evidence in the whole project, because it is measured in the actual
operating environment against the actual thing being replaced.

| Conditions | EcoSync advantage over iPhone |
|---|---|
| Clean air, open sky | **0.5 – 1 m better** |
| In scrub, under canopy | **2.5 – 3 m better**, sometimes more |

**The gap widens exactly where the work happens.** In the open a handset does reasonably well, so
the margin is modest. Under canopy the phone's small, poorly-sited antenna and its reliance on
fused Wi-Fi/cell/motion data fall apart, while a dedicated multi-constellation receiver with a
proper antenna holds up. Boxthorn in gullies and scrub is precisely the canopy case.

**Lead with this shape, not a single headline number.** "Two metres" invites an argument about
whether it is CEP or 95%. "Two-and-a-half to three metres better than the phone you are using
today, in the scrub where you actually work" is a claim from a side-by-side comparison, and it is
far harder to dispute.

**Honest caveat to keep in the back pocket:** these are *reported* accuracy figures, not surveyed
truth. Handset figures are known to be optimistic because the phone fuses non-GNSS sources, so
the real gap is more likely wider than narrower. If DEW presses, the answer is a controlled test
against a known mark — worth doing before any large commitment.

**And it strengthens the F10 case rather than weakening it.** Under canopy the dominant errors are
multipath and attenuation, which is exactly what dual-band L1/L5 is designed to mitigate. The
current L1-only M9 already wins there; the F10 should widen the gap further.

---

**Precision Positioning for the Apps You Already Use:** Both EcoSync units double as a
high-accuracy positioning source for the mobile workflows a board already runs. Operators
continue working in ArcGIS QuickCapture or Survey123 exactly as they do today — same project,
same fields, same submission path — while the app draws its position from the unit's dedicated
GNSS receiver instead of the handset's. Records land in the existing database at a materially
better position than the 3-to-8-metre drift typical of phone GPS.

For vehicle crews, the EcoSync Field already performs this alongside its spray logging: a
single unit records the treatment automatically and simultaneously improves the accuracy of any
observation the operator captures by hand. For foot-based work, the EcoSync Micro brings the
same capability off-vehicle.

*(Pick the figure to match the fitted module: **about two metres** for units running the current
L1 receiver, **about one metre** once the F10 dual-band module is fitted. Do not quote one
number across both.)*

No new data system, no schema migration, no retraining. The workflow is untouched; only the
positional quality changes.

---

*Why the Field unit matters most here:* it is the unit in the field now, so this is
demonstrable rather than promised. It also means a board can trial the accuracy improvement
against their own existing project before committing to anything else in the platform.

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

**1. Periodic export (CSV / File Geodatabase).** Simple, no integration. But it is manual,
stale by definition, kills the live-dashboard claim, and puts a person back in the chain of
custody the pitch attacks.

**2. Direct writes into the board's own layer.** What the schema-matching work was for. Needs
their field definitions, credentials, and — if DEW is on Enterprise behind a firewall — network
access a field unit may simply not have. Highest effort, highest fragility.

**3. Scheduled ETL from your layer into theirs.** MSA keeps its own hosted layers as now; a
scheduled job maps and copies into the board's. Schema mapping is done once, server-side, under
your control. Robust, but it is still a copy with a lag, and it is infrastructure you must run
and maintain.

**4. Share a hosted feature layer view — the one to lead with.** In ArcGIS Online you create a
**view layer** over your existing spray layers, filtered to that board's job numbers or region,
and share it with their organisation or a group. They add it to their own maps and dashboards
and see it live. No copy, no lag, no ETL to maintain, no schema migration on their side, and
**you retain ownership of the underlying data** — which matters commercially when the contract
comes up for renewal.

**Recommendation:** lead with option 4, keep option 3 in reserve for a board that insists the
records must physically reside in their own database, and treat option 1 purely as an ad-hoc
convenience — never as the architecture.

Worth confirming: whether their ArcGIS Online organisation can consume a view layer shared from
outside their org, or whether policy requires data to sit inside their own Enterprise portal.
That single question decides between option 4 and option 3, and it is a good thing to ask DEW
early — it is a five-minute answer for them and it shapes everything downstream.


---

## To verify before promising it

- ~~Confirm the Network connection option appears and accepts the unit.~~ **DONE 2026-08-26** —
  both **QuickCapture and Survey123** connected over TCP to the Field unit. No MFi, no Bluetooth.
- ~~Confirm which **NMEA sentences** are flowing.~~ **DONE 2026-08-26** — the unit emits
  `RMC VTG GGA GSA GSV GLL GST`. **GST is present**, so the accuracy estimate reaches the app.
  (It took a firmware fix: the enable request is now retried and its result checked, and the
  message config is persisted so it survives a power cycle.)
- **Still to do: test against a real DEW QuickCapture project**, not just a generic one.
- Check whether the board's operators use **Field Maps** as well — that one *does* need MFi,
  and the answer there is "not supported", not a workaround.
- Confirm QuickCapture displays and records the **accuracy value** from the external receiver,
  since that is what evidences the claim in an audit.

---

## Sources

- [ArcGIS QuickCapture — Use a high-accuracy receiver](https://doc.arcgis.com/en/quickcapture/help/highaccuracyuse.htm)
- [SparkFun RTK Everywhere — iOS connection methods](https://docs.sparkfun.com/SparkFun_RTK_Everywhere_Firmware/gis_software_ios/)
- [ArcGIS Field Maps — Prepare for high-accuracy data collection](https://doc.arcgis.com/en/field-maps/latest/prepare-maps/high-accuracy-data-collection.htm)
