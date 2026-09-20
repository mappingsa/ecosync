# EcoSync — should this become a platform?

A strategy note. Written 18 Sep 2026, revised 20 Sep after three things changed the picture: the
spray-record obligation, Rapid Logix's pricing, and a side-by-side comparison of the two reports.

**The question:** EcoSync currently syncs to ArcGIS Online, which ties it to customers who already
own Esri licences. Should MSA build the full stack — database, app, mapping platform — and sell to
anyone?

**The answer:** own the hardware and the data contract. Do not build a mapping platform. There is
a middle layer worth owning, it is far smaller than a platform, a competitor has proved customers
will pay for it, and their own report shows a regulated field they structurally cannot produce.

---

## 1. What is actually scarce

| Layer | Status |
|---|---|
| Database | Solved. Postgres/PostGIS, free, mature. |
| Mapping platform | Solved several times — Esri, QGIS, Mapbox, MapLibre, Felt, Fulcrum, Mergin. All funded teams. |
| Mobile capture app | Solved. QuickCapture, Survey123, QField, Fulcrum, Mapit. Several free. |
| **The unit** | **Not solved well by anyone.** |

The EcoSync Field is a rugged, trigger-integrated logger at roughly $300 of parts that holds
**2.16 m under heavy canopy where a phone gives 5.70 m** — 36 paired readings, 35 wins.

**Own the scarce layer, plug into the commodity layers.** That is what the NMEA-over-TCP work has
been doing: each app the unit learns to feed widens the market without MSA maintaining a
destination for the data.

### The niche, and why the big players left it

Precision agriculture solved spray logging — for **boom sprayers**. Trimble, Raven, TeeJet and the
machinery vendors own rate control on a tractor. None of them serve **spot treatment**: an operator
with a hose reel walking into scrub after individual plants. There is no machine to attach to, so
it is structurally unattractive to a machinery vendor.

---

## 2. The product is evidence, and the spray record is its specification

Contractors applying agricultural chemicals in South Australia are **legally required** to keep
records. Under the *Agricultural and Veterinary Products (Control of Use) Regulations 2017*,
Group 4 herbicide records must be kept **two years** and a copy **given to the property owner or
manager**.

That is not an abstraction. It is a specification, written by someone else, that cannot drift.

### What EcoSync already captures against it

| Required field | EcoSync today |
|---|---|
| Date applied | **Automatic** |
| Spray time, start and finish | **Automatic** |
| Paddock location / identification | **Automatic — to a metre, with accuracy evidence** |
| **Area treated (ha)** | **Automatic — computed from the logged perimeter** |
| Travel speed | **Automatic — GPS speed** |
| Crop or situation | Target species field |
| Product name(s) incl. adjuvants | Config / per job |
| Product rate(s) per hectare | Config |
| Equipment type, nozzle, pressure, spray volume | Config — set once |
| User name, address, contact | Config — set once |
| Property owner details | Per job |
| **Wind speed, start and finish** | **Missing** |
| **Wind direction, start and finish** | **Missing** |
| **Temperature, start and finish** | **Missing** |

*(Relative humidity and Delta T are explicitly not mandatory.)*

**EcoSync automatically captures the fields hardest to do honestly by hand.** "Paddock location" on
a paper form is a paddock name; EcoSync gives a polygon with per-record accuracy. Area treated is
normally an estimate; EcoSync computes it from a logged perimeter. Times are normally remembered;
EcoSync records them at the trigger.

A quality difference, not a convenience one.

### The weather gap, and the feature that closes it

Wind speed, direction and temperature at start *and* finish are the fields most often reconstructed
from memory at the end of the day — and the ones a regulator cares most about, because they are the
drift evidence.

**The unit already knows when a job starts and ends** — first trigger pull, auto-timeout close. It
can *prompt* at exactly those moments. That turns the weakest part of a spray record into the most
reliable part, and it would make MSA's record **more defensible than the incumbent's**, because
theirs is still typed in afterwards.

Entry from the operator's handheld meter, because an on-site observation is what is defensible. A
Bureau of Meteorology lookup at sync time can sit alongside as a cross-check, but the nearest
station is not the paddock and should not be presented as the record.

### Design constraint

Make the record template **data-driven, not hardcoded**. SA Group 4 is the strictest set; general
chemical-use records differ, and NSW, Victoria and the pest-controller rules have their own. If the
field list is configuration, one product serves every state. Hardcoded, it is a South Australian
product.

> **Not legal advice.** The list above is the Group 4 requirement specifically. Confirm with PIRSA
> (Agricultural and Veterinary Chemicals hotline, 1300 799 684) before building to it.

---

## 3. The competitor: Rapid Logix

Rapid Logix (Rapid Spray) sells the **Universal Flow Tracker** — a gun-mounted logger with a flow
sensor, GPS, three preset weed buttons and 4G upload — plus a reporting portal.

**MSA used it and stopped, on cost.** A churned customer of the incumbent, now building the
alternative, is the most valuable competitive position available.

### Their published pricing

| | |
|---|---|
| UFT hardware | **$2,299 AUD** per device, one-time |
| Contractor (1–4 devices, 6 logins) | **$2,496/yr** |
| Professional (5–8 devices) | $5,499/yr |
| Corporate (9–15 devices) | $10,549/yr |
| Enterprise+ (16+) | On application |

**One device, first year: $4,795. Every year after: $2,496.**

The market is proven — someone runs a business at those prices. And there is a validated segment
below them: MSA is itself an example.

### They publish no GNSS accuracy figure

For a product whose entire value is evidence of *where* a treatment happened, that silence is the
opening.

---

## 4. The two reports, side by side

*Comparing MSA's own report (30 Aug 2026, Birdseye Highway) against a Rapid Logix report
(1 Jul 2025, Bairds Bay), both against the SA Group 4 field list.*

| Required field | MSA report | Rapid Logix report |
|---|---|---|
| Date applied | ✓ | ✓ |
| Start and finish time | ✓ | ✓ |
| Location / identification | ✓ map | ✓ map + full address |
| Product names incl. adjuvants | ✓ | ✓ |
| Equipment type | ✓ | ✗ |
| Operator name and contact | ✓ | name only |
| **Wind speed, start and finish** | **one reading only** | **✓ both** |
| **Wind direction, start and finish** | **one reading only** | **✓ both** |
| **Temperature, start and finish** | **one reading only** | **✓ both** |
| **Area treated (ha)** | **✗** | **✗ — prints "N/A"** |
| Nozzle type and flow rate | ✗ | ✗ |
| Spray pressure | ✗ | ✗ |
| Spray volume (L/ha) | ✗ | ✗ |
| Travel speed | ✗ | ✗ |
| Product rate per hectare | ✗ | concentration only |
| Operator signature | ✗ | ✓ |

**Neither report is fully compliant.** The incumbent, at $4,795 in year one, is missing five
required fields.

### The finding that matters most

**Rapid Logix prints "Area Sprayed: N/A".**

They cannot compute it. A flow meter measures *how much left the tank*; it cannot measure *how much
ground was covered*. Area treated in hectares is a required field, and the market leader leaves it
blank.

**EcoSync computes it from the logged perimeter.** That is not a feature gap — it is a regulated
field their architecture cannot reach and EcoSync's already does. It is the single strongest
technical argument MSA has against them, and it comes from their own document.

### What they do better

- **Start and finish weather in a proper table.** MSA has one reading. Biggest compliance gap.
- **Chemical usage computed in millilitres** — 1263.80 ml Glyphosate 450, 252.76 ml Brushwet,
  derived from flow × concentration. More defensible than a hand-entered daily total.
- **An operator signature line.** PIRSA's own form has one. Costs nothing to add.
- **Graduated map symbols** sized by volume applied.

### What MSA does better

- **It reads like a deliverable to a client**, not a database export — header photograph, narrative
  field observations, dashboard link. For a board paying for the work, this is the document they
  would rather receive.
- **The map colours by control method** (spray vs cut-and-swab), carrying information about *what
  was done*, not just what was targeted. Rapid Logix colours by species only.

### What neither shows

**Positional accuracy.** EcoSync carries `gps_accuracy_m` on every record and it appears nowhere in
the output. The one number that distinguishes this data from everyone else's is currently invisible
in the deliverable.

### Corrections to the current MSA report

- "Vehicle Mounted **Foiler** Spray Rig" → **Foliar**
- "**Vigillant**" → **Vigilant**

Both on a document that goes to a government client.

---

## 5. On flow measurement

**Correction to an earlier draft of this note:** MSA is *already compliant*. The regulated field is
"spray volume (**litres per hectare**)" — a rate. Daily tank usage ÷ area treated satisfies it, and
EcoSync already computes the area. A flow meter is **granularity and automation, not legality**.

MSA removed the flow meter from the original build because it sat separately from the logger and
was awkward to plumb into the spray line. That reason still stands.

### The approach worth trying before plumbing anything

The unit already knows how long the trigger was held. With nozzle flow rate at working pressure as
a config value:

> **volume ≈ trigger-open time × nozzle flow rate**

Zero hardware, zero plumbing, no pressure-rated fitting, no leak path. And because MSA already
records the **daily tank total**, the sum of estimates can be reconciled against actual usage every
evening, giving a continuously self-correcting calibration factor.

Not certified-accurate — pressure varies, nozzles wear — but sufficient for per-treatment relative
volume, and upgradeable to a real inline meter later for customers who need certified numbers.

### Who actually needs per-treatment volume

| | Board | Contractor |
|---|---|---|
| Compliance | Already met by tank total ÷ area | Same |
| Chemical inventory reconciliation | Marginal | **Useful** |
| Invoice / dispute evidence | No | **Useful** |
| Actual vs label rate checking | Some interest | **Useful** |
| Per-plant volume for efficacy analysis | **Genuinely interesting** | Low |
| Volume at a specific location near water | **Potentially required** | **Potentially required** |

**Question to put to the board on 26 September**, in this order — not "would you like volume data?",
which teaches nothing:

> *"What do you currently report on chemical use, and who receives it?"*
> *"Is the daily total enough, or does anyone ever need to know how much went on at a particular
> location?"*
> *"Are there sites — near watercourses, sensitive areas — where you'd need to show the amount
> applied at a specific point?"*

A clear "the daily total is fine" is a useful no: it saves building something and says the
contractor market is where volume matters.

---

## 6. Market segments

| Segment | Driver | Speed to sell | Verdict |
|---|---|---|---|
| **Weed control contractors** | Legal record obligation, invoice evidence | Fast — owner decides | **Primary. MSA is its own first customer.** |
| **Linear infrastructure vegetation** | Regulatory clearance, bushfire risk | Medium | **Highest value** |
| Government NRM / Landscape Boards | Reporting to funders | Slow, procurement | In progress — reference customer |
| Local councils | Ratepayer accountability | Slow | Follows contractors |
| Biosecurity response | Chain of evidence for eradication | Episodic | Opportunistic |
| Revegetation / biodiversity credits | Provable treatment history | Emerging | Watch |
| Broadacre agriculture | Chemical use records | — | **Avoid** |

**Contractors are primary.** The obligation is legal not discretionary, the owner decides this
week, there is no GIS or IT department to negotiate with, and Rapid Logix has established the price
ceiling. **MSA is its own first customer** — the workflow does not have to be guessed.

**Avoid broadacre.** Deere, Trimble Ag, Agworld and AgriWebb own chemical application records with
machinery integration behind them.

---

## 7. A ladder, where every rung needs a customer

### Rung 0 — the unit works with everything *(in progress)*

NMEA over TCP, BLE via Nordic UART, Android mock location. Free market expansion, nothing to
maintain. Highest return per hour available now.

### Rung 1 — the spray record generator

**The clearest product in the plan, and MSA is the first user.** One button, one day's work, one
compliant record.

**Target specification — MSA's document, their rigour, plus what neither has:**

| Source | Fields |
|---|---|
| **Automatic from the unit** | date, start and finish times, location, **area treated (ha)**, travel speed, target species, control method, **positional accuracy per record** |
| **Prompted at the right moment** | wind speed, direction, temperature — **at first trigger pull and at close**, not from memory |
| **Config, set once** | operator name/address/contact, equipment type, nozzle and flow rate, spray pressure, product names and rates |
| **Per job** | job number, client, property owner and contact, narrative field observations |
| **Keep from the current report** | header photograph, prose observations, dashboard link, map coloured by control method |
| **Adopt from Rapid Logix** | start/finish weather table, computed chemical usage, **operator signature line** |

Output: PDF plus the map, and a copy to the property owner as the regulations require.

Scope is set by legislation, so it cannot drift.

*Gate: already met. Build it for MSA, use it a season, then sell it.*

### Rung 2 — hosted ingest, schema, exports

Units post records; one clean model; fan-out to an ArcGIS view layer, GeoPackage, OGC API feed,
webhook or scheduled CSV. Plus fleet state — which units, what firmware, battery, last sync.

*Gate: one paying customer who cannot be served by a file, or roughly ten units in the field.*

### Rung 3 — a viewer

Only after customers repeatedly ask "where do I look at it" *having used rungs 1 and 2*. Even then,
embed MapLibre.

*Gate: the same request from three separate customers.*

---

## 8. What not to build

**A mapping platform.** Funded competitors, solved category, at the cost of the only thing with a
moat.

**A capture app duplicating QuickCapture or QField.** They exist, several are free, customers
already have them.

**A portal competing with Rapid Logix on features.** They have a head start and a team. Compete on
**accuracy, area, price and data ownership** — not on dashboard count.

**Anything at all before 26 September.**

---

## 9. The risk worth naming

The commonest way a good hardware idea dies is the founder building software instead of selling
hardware. Software is comfortable: it happens at a desk, it feels like progress daily, and nobody
says no while you are doing it. Meanwhile the thing with the moat does not ship.

Rung 1 is the exception that proves the rule — it is software, but MSA is the customer, uses it
daily, and will know within a season whether it is right. That is not speculative building.

---

## 10. Questions still open

**Do you want to run a software business?** A hosted service is a permanent operational commitment
— uptime, backups, patching, support, privacy obligations, someone reachable on a Sunday. A choice,
not a consequence of success.

**What does capacity look like?** One person on firmware, enclosure, field testing, statistics,
documentation and sales — plus a partner arriving, plus the weed-identification tool competing for
the same hours.

**What does support cost per unit per year?** Rapid Logix sets the reference price. The open
question is not what to charge but what it costs to serve — the figure most often left out, and the
one that decides whether a low price is sustainable or a trap.

**What happens to the data if MSA stops?** A government client will ask. Export on demand, open
formats, no lock-in — honest *and* a competitive advantage over a portal vendor.

**Is the weed-identification tool the same business?** Shares the unit, schema and customer → it
strengthens this. Needs its own hardware, model training and buyer → it is a second company
competing for the same founder.

---

## 11. Recommendation

1. **Ship to the boards.** The 26th is the priority.
2. **Keep widening app compatibility.** Free market expansion.
3. **Build the spray record generator for MSA's own use.** Legally specified scope, immediate user,
   no speculation.
4. **Put area treated and positional accuracy on the face of it** — the two fields the incumbent
   cannot produce and MSA already holds.
5. **Add flow estimation from trigger time**, reconciled against the daily tank total. Plumb a real
   meter only if a customer needs certified volume.
6. **Sell to two or three contractors** with the record generator in hand — ideally ones who looked
   at Rapid Logix and baulked.
7. **Build rung 2 only when one of them pays and cannot use a file.**
8. **Do not build a mapping platform.** Probably ever.

The positioning line:

> **Rapid Logix tells you how much you sprayed. EcoSync tells you exactly where, how much ground
> you covered, and how accurately it was measured.**

And the strategic one:

> **MSA makes the instrument. The market already owns the map.**

---

## Appendix — still to collect

- Confirmation of the exact record obligation from PIRSA
- Two conversations with contractors who are not MSA
- A known-mark accuracy test, to replace reported accuracy with measured displacement

*Sources: PIRSA Group 4 herbicide record-keeping requirements; PIRSA "Comply with legislation";
Rapid Logix published pricing and UFT hardware pages, accessed 18 Sep 2026; MSA Spray Record Form
30/08/2026; Rapid Logix Chemical Spray Sheet, Bairds Bay, 01/07/2025.*
