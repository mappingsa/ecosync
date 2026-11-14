# EcoSync — Meeting Brief
**Landscape Board · Friday 9 October 2026**

---

## The one-line pitch

EcoSync captures weed treatment and survey activity as accurate spatial data at the moment
of work, with no app and nothing for the operator to remember — and delivers it into the
Board's GIS in the Board's own schema.

---

## What exists today (proven)

**EcoSyncField** — spray-gun integrated logger. **In service now.**

| | |
|---|---|
| Records logged | 3,100+ across 5 field days |
| Best day | 1,450 records in one day |
| Average accuracy | 1.5 – 1.7 m |
| Worst single reading (best day) | 2.8 m |
| Within 3 m | 100% (best day) |
| vs iPhone, same position same moment | 2.2 m vs 5.2 m — EcoSync better in 35 of 36 readings |
| Runtime | ~28 h per charge (from measured 2.8 %/h draw) |

Every record stores **its own accuracy value** — auditable per point, not just summarised.
Area treated is calculated automatically, which is the regulated figure most point-logging
apps leave blank.

**Honest framing:** two units built, both in service, five days of real operational data.
This is a working system, not a prototype — but it is early, and the numbers above are all
of them.

---

## What is in development (say so plainly)

| Product | Status |
|---|---|
| **EcoSyncMicro** — standalone wearable point logger | Concept. Design complete, not yet built. All figures are design targets. |
| **EcoSyncID** — AI weed detection from vehicle imagery | In development. Boxthorn first. All specifications are design targets. |

If asked "when" — give a range you can hold, not a date you can't.

---

## Getting data into the Board's ArcGIS Enterprise

**The transport is the easy part. The schema is the decision.**

Capturing in their field structure from day one is right, and it is the single most
important thing to settle in this meeting. Transport can be changed later for almost
nothing; a year of records in the wrong schema cannot.

### Four delivery options, least to most integrated

**1. File handover** — GeoJSON / File Geodatabase / Shapefile, matched exactly to their
schema, emailed or dropped to them.
*Simple, no credentials, no network dependency. Manual and batch — fine for monthly
reporting, poor for operations.*

**2. Live shared layer** *(recommended starting point)* — data lands in an MSA-hosted
feature layer built to their schema; they add it to their Enterprise maps as a remote layer.
*Live, no file shuffling, no access to their system required. Already demonstrated with DEW.
Needs their portal to be allowed to consume an external layer.*

**3. Distributed Collaboration** *(the proper long-term answer)* — Esri's built-in
org-to-org sync between ArcGIS Online and ArcGIS Enterprise. Scheduled or on-demand,
one-way or two-way.
*This is the method their GIS administrator will recognise and trust. Both admins set it up
once. Esri-native, no custom code.*

**4. Direct write into their Enterprise feature service** — EcoSync posts straight into
their system.
*Not recommended. Requires their portal reachable from the field (usually it isn't —
internal network or VPN), a service account, token lifetime management, and a security
review. Makes MSA dependent on their uptime and their schema changes.*

### The recommendation to put to them

> Capture in your schema. Hold it in our layer. Deliver it however suits your GIS team —
> file, live layer, or Esri Distributed Collaboration. Start with a live shared layer and
> move to Collaboration once it's proven.

---

## Ask them these four questions

1. **Can your GIS officer send the feature class definition you want us to match?**
   Field names, types, and any coded-value domains — especially the species list.

2. **What datum is your weed data in — GDA94 or GDA2020?**
   This matters more than it sounds. EcoSync logs WGS84. Against **GDA2020** that's a near
   match. Against **GDA94** there is a systematic shift of roughly **2 metres** — larger
   than our accuracy — so a transformation has to be applied. Getting this wrong puts every
   point consistently in the wrong place and nobody notices for a year.

3. **Is your Survey123 / portal Esri-hosted or on-premises, and is it reachable from the
   field?** Determines whether live delivery is even possible, and rules some reporting
   features in or out.

4. **Who owns the data when a contractor captures it, and how long must it be retained?**
   Settle it now, in writing, before the first unit ships.

---

## Likely hard questions — and the honest answer

**"How accurate is it really?"**
1.5–1.7 m average, 2.8 m worst on our best day, 100% within 3 m. Every point carries its own
accuracy value so you can filter on it. No subscription or correction service required.

**"What if the operator forgets?"**
There is nothing to remember. Pull the trigger, it logs.

**"What happens if it loses signal or power?"**
Logging is to onboard SD card and survives power loss. Sync happens when it's back in range.

**"Can it do RTK / centimetre accuracy?"**
Not on current hardware, and deliberately so — it would mean a correction subscription. The
next receiver revision can, and SouthPAN improves the free baseline over the next few years.

**"Is EcoSyncMicro / EcoSyncID available?"**
No. Micro is designed but not built. ID is in development, boxthorn first. Everything proven
today is EcoSyncField.

---

## Don't

- Don't quote a delivery date for Micro or ID.
- Don't claim centimetre accuracy, or SouthPAN improvement you haven't measured.
- Don't agree to write into their Enterprise system directly in the room.
- Don't give a per-unit price without knowing quantity and who supports it in the field.

---

*Mapping Services Australia · prepared 8 October 2026*
