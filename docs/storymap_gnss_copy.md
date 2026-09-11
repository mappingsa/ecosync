# MSA EcoSync StoryMap — GNSS copy

Drop-in replacement text for the *MSA EcoSync Platform* StoryMap.
Written to sit in your existing voice. Every figure is sourced in the appendix.

Revised **11 Sep 2026** against three days of paired field measurement.

> **This file is the single source of truth for StoryMap copy.**
>
> Hardware is the **u-blox F10 dual-band (L1/L5) platform**. **Do not name a specific part
> number in published copy** — it makes the pitch hostage to stock.
>
> **Never write "sub-metre"**: 1.0 m CEP is about 2.1 m at 95% confidence, and a GIS reader will
> read it as a 95% figure. **Never write "survey-grade"** — it implies centimetres.
>
> *The earlier prohibition on "approximately one metre", "dual-band" and "SouthPAN" applied to
> the M9 module originally fitted. It is withdrawn — those claims are now supported by both the
> datasheet and three days of field measurement.*
>
> `gnss_hardware_decision.md` holds the reasoning; the wording lives here.

---

## OUTSTANDING FIXES in the published StoryMap

Do these first — they are live and visible right now.

| Priority | Issue |
|---|---|
| **1** | **Three remaining instances of "sub-metre" / "sub-meter"** → change to **"metre-level"** |
| **2** | "**survey-grade**, sub-meter accuracy" in Section 2 (Micro) → drop "survey-grade" entirely |
| 3 | Image caption: "attached to the **spay** gun" → spray |
| 4 | Image caption: "EcoSync **File** Touch-Pad" → Field |
| 5 | Section 7: "on **there** own active spray rigs" → their |
| 6 | Section 2 (Micro): "**and or** tracking revegetation" → and tracking |
| 7 | Section 1: "Standard Data **loggers**… Standard Data Logger Server" → inconsistent capitalisation |

---

## A. REPLACES the two hardware bullets in Section 2 → *Hardware Architecture*

**EcoSync Field (Current Pilot Unit):** A heavy-duty, universally adaptable spatial logger.
Utilising a modular trigger and touchpad system, a single EcoSync Field unit seamlessly
integrates with a 4WD's automatic hose reel, but can also be instantly swapped onto a standard
backpack sprayer. This completely eliminates the need for the separate "High" and "Low" hardware
models required by traditional spray data loggers.

**EcoSync Micro (In Active Development):** A compact, wearable form-factor engineered for
off-vehicle data capture. Unlike consumer phones running mobile apps, the Micro houses a
dedicated multi-band GNSS module to eliminate the 3-to-8-metre drift typical of handset
positioning. It brings metre-level accuracy to foot-based operations — purpose-built for logging
manual treatments (cut-and-swab, hand-pulling), mapping weed and pest observations, scoring tree
health, and tracking revegetation efforts.

**A Single Positioning Platform:** Both units are built around the same u-blox F10 dual-band
(L1/L5) GNSS platform. This is deliberate. Rather than locking each product to a fixed accuracy
grade, the same hardware supports multiple tiers of correction, so the board selects the
precision each task actually warrants — without buying different devices.

**Unified Data Alignment:** Both the EcoSync Field and EcoSync Micro capture and transmit data in
a completely synchronised format. Whether a point or polygon is logged via the vehicle rig or on
foot via the Micro, it feeds into the exact same ESRI database schema. This ensures data
uniformity across the entire operation, blending high-volume vehicle spraying and precision
foot-treatments onto a single, reliable map layer.

---

## B. NEW subsection — insert directly after *Hardware Architecture*

### Positioning Accuracy: Sovereign, Subscription-Free

Positioning accuracy is not a fixed property of the hardware — it depends on the correction
source feeding it. Because both EcoSync units run a multi-band L1/L5 receiver, they can draw on
whichever correction stream suits the work, with no change of equipment.

**Tier 1 — SouthPAN (Standard, included).** SouthPAN is the Australian and New Zealand
Government's satellite-based augmentation network, operated by Geoscience Australia and Toitū Te
Whenua Land Information New Zealand. It is delivered directly from satellite, open-access and
**free to use** — no base station, no mobile coverage, no subscription and no ongoing cost to the
board. In this configuration EcoSync achieves **approximately one metre horizontal accuracy
(1.0 m CEP)**, against the 3 to 8 metres typical of handset GPS. It works in scrub, in gullies
and well outside mobile range.

**Tier 2 — Centimetre-grade RTK (Roadmap).** The platform's L1/L5 architecture supports RTK
correction for work that genuinely warrants centimetres — permanent monitoring plots,
boundary-adjacent treatment, revegetation compliance. This requires a correction service and a
live data link, and is offered as a per-project option rather than standard fitment.

**Why this matters for compliance.** Consumer handset positioning drifts 3 to 8 metres. At that
error budget a Weed of National Significance recorded this season cannot be reliably matched to
the same plant next season, and a treated polygon cannot be defended in an audit. Metre-level
positioning turns a field record into a durable spatial asset — one that supports genuine
year-on-year re-treatment tracking, density trend analysis, and evidence-grade reporting.

**Roadmap note.** SouthPAN currently operates as an early Open Service. Its dual-frequency (DFMC)
and precise point positioning services are progressively improving as Geoscience Australia
deploys infrastructure upgrades, with Full Operating Capability scheduled for 2028. Because
EcoSync already runs L1/L5-capable receivers, both units will inherit those improvements as they
come online — with no hardware replacement and no cost to the board.

---

## B2. NEW subsection — the measured evidence

*This is the strongest material in the entire pitch. It should sit immediately after B, and if
anything in the StoryMap deserves a chart, it is the canopy table.*

### Measured in the Field, Against the Device It Replaces

Claims about positioning accuracy are easy to make and hard to check. So EcoSync was tested the
only way that settles the question: side by side with an iPhone, same moment, same place, across
three working days in real infestations.

**Thirty-six paired readings. EcoSync was more accurate in thirty-five of them.**

| | EcoSync | iPhone |
|---|---|---|
| Average positional accuracy | **2.16 m** | 5.22 m |
| Current hardware only | **1.82 m** | 5.10 m |
| Best single reading | **0.98 m** | 2.80 m |

**But the average is not the point. This is:**

| Conditions | EcoSync | iPhone | EcoSync advantage |
|---|---|---|---|
| Open sky | 1.37 m | 2.80 m | 1.4 m better |
| Light canopy | 1.44 m | 5.00 m | **3.6 m better** |
| Medium canopy | 2.02 m | 5.55 m | **3.5 m better** |
| Heavy canopy | 2.16 m | 5.70 m | **3.5 m better** |

**Moving from open sky into heavy canopy, the handset loses 2.90 metres of accuracy. EcoSync
loses 0.79 metres.** It holds up more than three times better in the conditions where weed
control actually takes place.

In open ground a modern phone performs reasonably, and the margin is modest. Under canopy its
small internal antenna and its reliance on fused Wi-Fi, cellular and motion data fall away — and
a dedicated dual-band receiver does not. Boxthorn in gullies, blackberry along watercourses,
olive under mallee: these are canopy environments, and they are where the phone is weakest and
EcoSync strongest.

**Consistency matters as much as accuracy.** Across the same readings, EcoSync's results varied
by a standard deviation of 0.47 m; the handset's by 1.37 m. As the operator put it: *the logger
was mostly stable where the phone would jump around a lot.* For re-treatment tracking, a record
that lands reliably within a metre is worth more than one that averages well and scatters widely.

**SouthPAN augmentation was measured, not assumed.** Comparing readings with and without
augmentation applied, across two independent days, SouthPAN was worth **0.36 to 0.57 metres** and
roughly halved the variability. The correction is confirmed as actually in use by the receiver,
not merely available.

**All of the above was recorded with the unit mounted on a spray gun in ordinary working
use** — not on a tripod, not in ideal conditions, not by a technician.

---

## C. REPLACES roadmap item 1 in Section 5 → *Future-Proofing & Compliance Roadmap*

1. **EcoSync Micro Rollout:** Expanding real-time, metre-level tracking to off-track foot crews,
   backpack applicators and ecologists — bringing vehicle-grade spatial accuracy to work that
   currently relies on handset GPS.

*(Consider adding as item 4:)*

4. **Progressive Accuracy Gains:** As SouthPAN advances toward Full Operating Capability in 2028,
   deployed EcoSync units inherit improved correction services automatically — the platform gets
   more accurate over its service life rather than less relevant.

---

## D. Typo fixes

*Moved to the top of this file — see **OUTSTANDING FIXES**. Section letter retained so earlier
references to "section D" still resolve.*

---

## E. REPLACES the *Hands-Free "Smart" Polygons* bullet in Section 3

The current wording pins the pitch to a hardcoded "15 seconds". That figure is now an adjustable
setting, so quoting it commits you to a number a board may well want changed — and dates the copy
the moment anyone tunes it.

**Replacement:**

**Hands-Free "Smart" Polygons:** Operators never touch a screen in the dirt. Squeezing the
hardware spray trigger instantly drops a point or begins mapping a polygon perimeter. Smart
Auto-Timeout logic then closes and secures the boundary once the operator stops treating and
moves on — with the idle threshold, like every other operating parameter, adjustable from the
in-cab config portal to suit the density and layout of the infestation being worked.

**Why this is stronger:** it makes the same point without a number to defend, and it turns a
fixed behaviour into evidence of the configurability you argue for elsewhere in the story.

*(If you prefer a concrete figure for readability, "roughly fifteen seconds by default,
adjustable in the field" is accurate and still leaves you room to move.)*

---

## F. OPTIONAL — new bullet for Section 3, point de-duplication

**Intelligent Point De-Duplication:** A single large boxthorn can take twenty trigger pulls to
treat. Rather than stacking twenty records on one plant, the unit logs a new point only once the
operator has either paused long enough or physically moved to a different plant. Boards receive
one record per weed treated — not one record per squeeze — so density mapping and treatment counts
reflect reality. Both thresholds are adjustable from the config portal.

---

## F2. OPTIONAL — new bullet, positional quality on every record

**Accuracy Travels With the Data:** Every point and every polygon carries the positional accuracy
achieved at the moment of capture. Polygons record the *worst* accuracy observed anywhere along
the perimeter rather than an average, so a boundary that drifted partway round cannot present
itself as a clean one. Boards can filter, audit or exclude records on positional quality rather
than accepting a dataset on trust — and the field is added to their schema by the platform, not
demanded of them.

*Why this belongs in the pitch:* it converts accuracy from a marketing claim into a per-record
attribute a GIS officer can query. That is a materially different kind of assurance, and it is
the sort of thing an analyst will notice and value.

---

## G. Screenshots — what to show, and what not to

**Show the Field Setup page.** Seven fields, large touch targets, obviously usable one-handed on a
phone. Direct evidence for the claim already made in Section 4 about updating targets and job
numbers from the cab.

*Suggested caption:* "Field Setup — targets, control methods and job numbers, updated wirelessly
from the cab. No laptop, no coding, no IT support."

**Do not publish the Advanced Settings page.** Even with passwords masked, the screenshot exposes
the Wi-Fi network name, the existence and name of the setup access point, the administrator
username, and the full parameter list — useful reconnaissance for anyone who later stands within
range of a unit. It also works against the friction argument: a board manager seeing fifteen
technical fields concludes the system needs IT support to run.

Keep that screenshot for a technical brief to DEW, and redact the SSID and username first.

### Suggested new bullet for Section 4 — *Wireless Administration & Local Configuration*

**Two-Tier Access Control:** Configuration is split by role. Operators reach a simple field page
to set target species, control methods and job numbers. The parameters that govern data quality —
positional accuracy thresholds, capture geometry, network credentials and API keys — sit behind
administrator authentication. Field staff cannot inadvertently widen an accuracy tolerance and
silently degrade a season of records, and credentials are never exposed on the screen operators
use daily. Boards get self-serve agility at the operator level without surrendering control of
data integrity.

*Why this belongs in the pitch:* for a government client this is a governance and compliance
point, not a technical detail. It answers the question a records manager will ask — "what stops a
contractor changing settings that affect the data we rely on?"

---

## Appendix — every claim, and where it comes from

Keep this. If DEW challenges a figure, you have the citation.

### Measured claims — MSA field testing

| Claim | Basis |
|---|---|
| 36 paired readings, 35 EcoSync wins | Field days 26 Aug, 10 Sep, 11 Sep 2026, EcoSync vs iPhone, simultaneous |
| EcoSync 2.16 m / iPhone 5.22 m pooled | Means of all 36 paired readings |
| EcoSync 1.82 m / iPhone 5.10 m, current hardware | 24 readings on the two F10 days |
| Best single reading 0.98 m | 11 Sep, light canopy, SouthPAN applied |
| Canopy degradation 0.79 m vs 2.90 m | 11 Sep, open-sky mean vs heavy-canopy mean, both devices |
| Standard deviation 0.47 m vs 1.37 m | 11 Sep, n = 12 |
| SouthPAN worth 0.36–0.57 m | 10 and 11 Sep, augmented vs unaugmented readings within each day |
| Augmentation confirmed in use | Firmware polls the receiver's differential-solution flag per reading |

**Method, if asked:** readings taken in pairs, same moment and same position, EcoSync mounted on
the spray gun in ordinary working use, iPhone held by the same operator. Conditions recorded per
reading (open / light / medium / heavy canopy) along with whether SouthPAN augmentation was
active.

**Known limitation, and say it first.** These are each device's *reported* accuracy estimate, not
displacement measured against a surveyed mark. Handset figures are generally optimistic because
the phone fuses Wi-Fi, cellular and motion data into its estimate, so the true gap is more likely
wider than narrower — but the honest framing is a comparison between two self-reported figures. A
controlled test against a known mark is the next step and is planned.

### Specification claims — external sources

| Claim in the copy | Source | Exact wording |
|---|---|---|
| SouthPAN is free / open-access | GA SouthPAN FAQ | "open-access and free to use with any compatible GNSS device" |
| 1.0 m CEP with SBAS; SouthPAN supported | u-blox NEO-F10N datasheet | "Position accuracy (CEP) 1 m"; SBAS list includes "SouthPAN" |
| Three services, DFMC + PVS on L5 | GA SouthPAN receiver docs | DFMC SBAS and PVS "delivered on the L5 frequency" |
| Full Operating Capability 2028 | GA SouthPAN FAQ | "Full Operating Capability (FOC) is scheduled for 2028" |

### Three figures deliberately NOT used

**"Sub-metre."** Independent testing (Spatial Source) described the SouthPAN L1 service as
providing sub-metre accuracy across a range of devices. That is an observation about the service,
not a specification this receiver can be held to. **1.0 m CEP is the 50th percentile — roughly
2.1 m at 95%, which is how a GIS officer will read it.** Our own measured mean of 1.82 m under
working conditions is consistent with the specification and inconsistent with a sub-metre claim.

**"As little as 10 centimetres."** This appears on GA's SouthPAN landing page, but their own FAQ
specifies PPP via SouthPAN at **≤0.40 m horizontal, ≤0.55 m vertical (95% confidence)**. Quoting
10 cm invites a correction from anyone at DEW who has read the service definition.

**PPP via SouthPAN as an operating mode.** The FAQ states full accuracy is reached "after a
convergence period better than 80 minutes during PVS early Open Services." A spray unit that
power-cycles between paddocks would rarely converge. PPP is genuinely unsuitable for this duty
cycle — which is why the copy above builds on the L1/DFMC services and offers RTK as the
centimetre path.

### Still not publicly quotable

Per-service accuracy for **DFMC SBAS** specifically is not stated on GA's public pages — they
direct readers to the Service Definition Documents. Independent testing was conducted while DFMC
was not yet operational, so no measured public figures exist. The copy above avoids quoting a
DFMC number for that reason. If you want one, pull it from the Service Definition Document and
cite it explicitly.

---

## Operational note for the user guide — not StoryMap copy

**Switch the unit on in the open, at the vehicle, and let it acquire before starting work.**

SouthPAN augmentation takes several minutes to establish, because the receiver must download a
full correction message set including the ionospheric grid. Once established it is held
stubbornly — on 11 Sep it did not drop once across an entire working day, including under heavy
canopy. But readings taken during the warm-up are unaugmented, and unaugmented readings are
0.36–0.57 m worse.

This costs nothing and is worth more than any hardware change currently on the table.

---

## Sources

- Geoscience Australia — SouthPAN: https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan
- Geoscience Australia — SouthPAN FAQ: https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan/southpan-frequently-asked-questions
- Geoscience Australia — SouthPAN GNSS Receiver Documentation: https://geoscienceaustralia.github.io/SouthPAN-GNSS-Receiver-Documentation/
- Spatial Source — Testing SouthPAN and commercial GNSS services: https://www.spatialsource.com.au/testing-southpan-and-commercial-gnss-services/
- u-blox NEO-F10N datasheet: https://content.u-blox.com/sites/default/files/documents/NEO-F10N_DataSheet_UBX-23002117.pdf
