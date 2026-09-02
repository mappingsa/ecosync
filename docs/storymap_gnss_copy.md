# MSA EcoSync StoryMap — GNSS upgrade copy

Drop-in replacement text for the *MSA EcoSync Platform* StoryMap.
Written to sit in your existing voice. Every figure is sourced in the appendix.

> **This file is now the single source of truth for StoryMap copy.** Hardware is the **u-blox
> F10 platform (DAN-F10N or NEO-F10N, both 1 m CEP with SouthPAN). **Do not name a specific
> part number in published copy** — it makes the pitch hostage to stock. **Never write
> "sub-metre"**: 1.0 m CEP is
> about 2.1 m at 95% confidence, and a GIS reader will read it as a 95% figure.
> `gnss_hardware_decision.md` holds the reasoning; the wording lives here.

---

## A. REPLACES the two hardware bullets in Section 2 → *Hardware Architecture*

**EcoSync Field (Current Pilot Unit):** A heavy-duty, universally adaptable spatial logger.
Utilising a modular trigger and touchpad system, a single EcoSync Field unit seamlessly
integrates with a 4WD's automatic hose reel, but can also be instantly swapped onto a
standard backpack sprayer. This completely eliminates the need for the separate "High" and
"Low" hardware models required by traditional spray data loggers.

**EcoSync Micro (In Active Development):** A compact, wearable form-factor engineered for
off-vehicle data capture. Unlike consumer phones running mobile apps, the Micro houses a
dedicated multi-band GNSS module to eliminate the 3-to-8-metre drift typical of handset
positioning. It brings approximately one-metre accuracy to foot-based operations — purpose-built
for logging manual treatments (cut-and-swab, hand-pulling), mapping weed and pest
observations, scoring tree health, and tracking revegetation efforts.

**A Single Positioning Platform:** Both units are built around the same u-blox F10 dual-band
(L1/L5) GNSS platform. This is deliberate. Rather than locking each product to
a fixed accuracy grade, the same hardware supports three tiers of correction, so the board
selects the precision each task actually warrants — without buying different devices.

**Unified Data Alignment:** Both the EcoSync Field and EcoSync Micro capture and transmit
data in a completely synchronised format. Whether a point or polygon is logged via the
vehicle rig or on foot via the Micro, it feeds into the exact same ESRI database schema.
This ensures data uniformity across the entire operation, blending high-volume vehicle
spraying and precision foot-treatments onto a single, reliable map layer.

---

## B. NEW subsection — insert directly after *Hardware Architecture*

### Positioning Accuracy: Sovereign, Subscription-Free

Positioning accuracy is not a fixed property of the hardware — it depends on the correction
source feeding it. Because both EcoSync units run a multi-band L1/L5 receiver, they can draw
on whichever correction stream suits the work, with no change of equipment.

**Tier 1 — SouthPAN (Standard, included).** SouthPAN is the Australian and New Zealand
Government's satellite-based augmentation network, operated by Geoscience Australia and Toitū
Te Whenua Land Information New Zealand. It is delivered directly from satellite, open-access
and **free to use** — no base station, no mobile coverage, no subscription and no ongoing cost
to the board. In this configuration EcoSync achieves **approximately one metre horizontal
accuracy (1.0 m CEP)**, against the 3 to 8 metres typical of handset GPS. It works in scrub,
in gullies and well outside mobile range.

**Tier 2 — Centimetre-grade RTK (Roadmap).** The platform's L1/L5 architecture supports RTK
correction for work that genuinely warrants centimetres — permanent monitoring plots,
boundary-adjacent treatment, revegetation compliance. This requires a correction service and a
live data link, and is offered as a per-project option rather than standard fitment.

**Why this matters for compliance.** Consumer handset positioning drifts 3 to 8 metres. At
that error budget a Weed of National Significance recorded this season cannot be reliably
matched to the same plant next season, and a treated polygon cannot be defended in an audit.
Metre-level positioning turns a field record into a durable spatial asset — one that supports
genuine year-on-year re-treatment tracking, density trend analysis, and evidence-grade
reporting.

**Roadmap note.** SouthPAN currently operates as an early Open Service. Its dual-frequency
(DFMC) and precise point positioning services are progressively improving as Geoscience
Australia deploys infrastructure upgrades, with Full Operating Capability scheduled for 2028.
Because EcoSync already runs L1/L5-capable receivers, both units will inherit those
improvements as they come online — with no hardware replacement and no cost to the board.

---

## C. REPLACES roadmap item 1 in Section 5 → *Future-Proofing & Compliance Roadmap*

1. **EcoSync Micro Rollout:** Expanding real-time, metre-level tracking to off-track foot crews,
   backpack applicators and ecologists — bringing vehicle-grade spatial accuracy to work that
   currently relies on handset GPS.

*(Consider adding as item 4:)*

4. **Progressive Accuracy Gains:** As SouthPAN advances toward Full Operating Capability in
   2028, deployed EcoSync units inherit improved correction services automatically — the
   platform gets more accurate over its service life rather than less relevant.

---

## D. Typo fixes spotted while reading

| Location | Current | Should be |
|---|---|---|
| Image caption | "attached to the **spay** gun" | spray |
| Image caption | "EcoSync **File** Touch-Pad" | Field |
| Section 7 | "on **there** own active spray rigs" | their |
| Section 2, Micro | "**and or** tracking revegetation" | and tracking |
| Section 1 | "Standard Data **loggers**... Standard Data Logger Server" | inconsistent capitalisation |
| Section 2, Micro | "**survey-grade**, sub-meter accuracy" | drop "survey-grade" — it implies centimetres and contradicts "sub-meter" |

---

## E. REPLACES the *Hands-Free "Smart" Polygons* bullet in Section 3

The current wording pins the pitch to a hardcoded "15 seconds". That figure is now an
adjustable setting, so quoting it commits you to a number a board may well want changed —
and dates the copy the moment anyone tunes it.

**Replacement:**

**Hands-Free "Smart" Polygons:** Operators never touch a screen in the dirt. Squeezing the
hardware spray trigger instantly drops a point or begins mapping a polygon perimeter. Smart
Auto-Timeout logic then closes and secures the boundary once the operator stops treating and
moves on — with the idle threshold, like every other operating parameter, adjustable from the
in-cab config portal to suit the density and layout of the infestation being worked.

**Why this is stronger:** it makes the same point without a number to defend, and it turns a
fixed behaviour into evidence of the configurability you argue for elsewhere in the story.

*(If you prefer to keep a concrete figure for readability, "roughly fifteen seconds by
default, adjustable in the field" is accurate and still leaves you room to move.)*

---

## F. OPTIONAL — new bullet for Section 3, if you want to cover point de-duplication

**Intelligent Point De-Duplication:** A single large boxthorn can take twenty trigger pulls
to treat. Rather than stacking twenty records on one plant, the unit logs a new point only
once the operator has either paused long enough or physically moved to a different plant.
Boards receive one record per weed treated — not one record per squeeze — so density
mapping and treatment counts reflect reality. Both thresholds are adjustable from the config
portal.

---

## G. Screenshots — what to show, and what not to

**Show the Field Setup page.** Seven fields, large touch targets, obviously usable one-handed
on a phone. It is direct evidence for the claim already made in Section 4 about updating
targets and job numbers from the cab.

*Suggested caption:* "Field Setup — targets, control methods and job numbers, updated
wirelessly from the cab. No laptop, no coding, no IT support."

**Do not publish the Advanced Settings page.** Even with passwords masked, the screenshot
exposes the Wi-Fi network name, the existence and name of the setup access point, the
administrator username, and the full parameter list — useful reconnaissance for anyone who
later stands within range of a unit. It also works against the friction argument: a board
manager seeing fifteen technical fields concludes the system needs IT support to run.

Keep that screenshot for a technical brief to DEW, and redact the SSID and username first.

### Suggested new bullet for Section 4 — *Wireless Administration & Local Configuration*

**Two-Tier Access Control:** Configuration is split by role. Operators reach a simple field
page to set target species, control methods and job numbers. The parameters that govern data
quality — positional accuracy thresholds, capture geometry, network credentials and API keys
— sit behind administrator authentication. Field staff cannot inadvertently widen an accuracy
tolerance and silently degrade a season of records, and credentials are never exposed on the
screen operators use daily. Boards get self-serve agility at the operator level without
surrendering control of data integrity.

*Why this belongs in the pitch:* for a government client this is a governance and compliance
point, not a technical detail. It answers the question a records manager will ask — "what
stops a contractor changing settings that affect the data we rely on?"

---

## Appendix — every claim, and where it comes from

Keep this. If DEW challenges a figure, you have the citation.

| Claim in the copy | Source | Exact wording |
|---|---|---|
| SouthPAN is free / open-access | GA SouthPAN FAQ | "open-access and free to use with any compatible GNSS device" |
| L1 service gives sub-metre | Spatial Source, independent testing | "operating well and providing sub-metre accuracy across a range of devices" |
| Three services, DFMC + PVS on L5 | GA SouthPAN receiver docs | DFMC SBAS and PVS "delivered on the L5 frequency", augmenting GPS L1/L5 and Galileo E1/E5a |
| Full Operating Capability 2028 | GA SouthPAN FAQ | "Full Operating Capability (FOC) is scheduled for 2028" |
| NEO-F9P is L1/L5 **and** RTK | u-blox announcement | "multi-band L1/L5 RTK", GPS + Galileo + BeiDou, "centimeter-level accuracy in seconds", ~50% smaller than ZED |

### Two figures deliberately NOT used

**"As little as 10 centimetres."** This appears on GA's SouthPAN landing page, but their own
FAQ specifies PPP via SouthPAN at **≤0.40 m horizontal, ≤0.55 m vertical (95% confidence)**.
Quoting 10 cm invites a correction from anyone at DEW who has read the service definition.
Sub-metre is defensible; 10 cm is not.

**PPP via SouthPAN as an operating mode.** The FAQ states full accuracy is reached "after a
convergence period better than 80 minutes during PVS early Open Services." A spray unit that
power-cycles between paddocks would rarely converge. PPP is genuinely unsuitable for this
duty cycle — which is why the copy above builds on the L1/DFMC services and offers RTK as
the centimetre path, rather than promising PPP.

### Worth verifying before you publish

Per-service accuracy for **DFMC SBAS** specifically is not stated on GA's public pages — it
directs readers to the Service Definition Documents. Independent testing was conducted while
DFMC was not yet operational, so no measured field figures exist publicly. The copy above
avoids quoting a DFMC number for that reason. If you want one, pull it from the Service
Definition Document and cite it explicitly.

---

## Sources

- Geoscience Australia — SouthPAN: https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan
- Geoscience Australia — SouthPAN FAQ: https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan/southpan-frequently-asked-questions
- Geoscience Australia — SouthPAN GNSS Receiver Documentation: https://geoscienceaustralia.github.io/SouthPAN-GNSS-Receiver-Documentation/
- Spatial Source — Testing SouthPAN and commercial GNSS services: https://www.spatialsource.com.au/testing-southpan-and-commercial-gnss-services/
- u-blox — two new high-precision F9 modules: https://www.u-blox.com/en/u-blox-announces-two-new-high-precision-gnss-positioning-modules-F9-platform
