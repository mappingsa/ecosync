# GNSS hardware decision — DAN-F10N vs NEO-F9P

Researched 26 Aug 2026 against Core Electronics AU pricing.

---

## Short answer

**The DAN-F10N is the right module for EcoSync, and you do not need RTK.**

The counterintuitive part: **without RTK corrections the expensive module is worse.**

| Module | Quoted CEP (50%) | ≈ 95% confidence | Price AUD | Antenna |
|---|---|---|---|---|
| DAN-F10N **with SBAS** | **1.0 m** | ~2.1 m | **$104.95** | patch included |
| DAN-F10N without SBAS | 1.5 m | ~3.1 m | — | — |
| NEO-F9P **without RTK** | 1.5 m | ~3.1 m | $439.95 | **not included** |
| NEO-F9P **with RTK** | 0.01 m | — | $439.95 | not included |

Because SouthPAN *is* SBAS, the F10N runs in its best mode for free, everywhere, with no
data link. The F9P only beats it once you are actually feeding it RTCM corrections — and
until then it sits at 1.5 m, half a metre worse, for roughly four times the price before
you have even bought an antenna.

---

## RTK needs three things you do not currently have

1. **An NTRIP client in firmware.** Real work — the device must pull RTCM from a caster and
   push it into the module continuously.
2. **A live data link in the paddock.** EcoSync only reaches Wi-Fi at sync time. RTK needs
   corrections *while* you are spraying. That means a cellular modem or a tethered phone.
3. **A CORS or network RTK subscription**, which is an ongoing cost per unit.

Miss any one and the F9P is simply a worse receiver that cost four times as much. Buy it only
when a specific job justifies centimetres and all three pieces are in place.

---

## Why the F10N suits this project specifically

- **SouthPAN is named explicitly** in its supported SBAS list, alongside WAAS and EGNOS.
- **The antenna is included** — a 20×20×8 mm dual-band patch, with a switch for an external
  one. The F9P needs an SMA antenna you buy separately, and a decent L1/L5 antenna is neither
  small nor cheap. For the **EcoSync Micro** especially, an integrated patch in that footprint
  is close to decisive.
- **Cost across a fleet.** At roughly $105 versus $550-plus per unit installed, this is the
  difference between a viable quote to a landscape board and an unviable one.

---

## Two things to change

### 1. Firmware: the F10N breakout is UART, not I²C

Your current code talks to the GPS over I²C (`Wire`, address 0x42). The SparkFun DAN-F10N
breakout exposes **UART only** — no Qwiic connector. The SparkFun library supports serial, so
this is a contained change:

```cpp
Serial1.begin(38400, SERIAL_8N1, RX_PIN, TX_PIN);
if (myGNSS.begin(Serial1) == false) { ... }
```

Everything downstream — `pollGPS()`, the caching, `gpsUsable()` — is unchanged, because it
only reads from the library. Expect an hour, not a day. A side benefit: it frees the I²C bus,
leaving only the battery gauge on it.

*(The NEO-F9P breakout has Qwiic and would be a drop-in with no code change. Worth knowing,
but not worth $335 and a worse fix.)*

### 2. The pitch copy needs correcting — this one matters

**CEP is the 50th percentile.** GIS and survey specifications are normally quoted at 95%.
A 1.0 m CEP is roughly **2.1 m at 95% confidence**.

So the current draft claim of "sub-metre accuracy" is **not supportable** with this module if
anyone at DEW reads it as a 95% figure — and a GIS officer will. Independent SouthPAN testing
did describe the L1 service as sub-metre, but that is a general observation about the service,
not a specification you can hold this receiver to.

Replace the Tier 1 and Tier 2 wording in section B of `storymap_gnss_copy.md` with the
following, which is honest and still strong:

> **Tier 1 — SouthPAN (Standard, included).** SouthPAN is the Australian and New Zealand
> Government's satellite-based augmentation network, operated by Geoscience Australia and
> Toitū Te Whenua Land Information New Zealand. It is delivered directly from satellite,
> open-access and **free to use** — no base station, no mobile coverage, no subscription and
> no ongoing cost to the board. In this configuration EcoSync achieves **approximately one
> metre horizontal accuracy (1.0 m CEP)**, against the 3 to 8 metres typical of handset GPS.
> It works in scrub, in gullies and well outside mobile range.
>
> **Tier 2 — Centimetre-grade RTK (Roadmap).** The platform's L1/L5 architecture supports
> RTK correction for work that genuinely warrants centimetres — permanent monitoring plots,
> boundary-adjacent treatment, revegetation compliance. This requires a correction service
> and a live data link, and is offered as a per-project option rather than standard fitment.

**Why this is still a winning claim.** You are not selling centimetres. You are selling the
difference between 3–8 m and about 1 m — which is exactly the difference between "a weed was
sprayed somewhere in this paddock" and "this individual boxthorn was treated, and here it is
again next season." That argument does not need RTK, and overclaiming would put every other
figure in the document under suspicion.

---

## ESP32 board: no change needed

The SparkFun Thing Plus ESP32 WROOM stays. It has spare UARTs for the F10N, the SPI bus for
the SD card and I²C for the fuel gauge, and the firmware currently uses a fraction of its
flash and RAM. Changing to an S3 would mean reworking pin assignments and re-testing
everything for no benefit you can currently name.

The one place a different board earns its keep is the **EcoSync Micro**, where the Thing Plus
footprint is the limiting factor for a wearable. That is a fresh enclosure and power design
rather than an upgrade to the Field unit — worth treating as its own exercise, with the
F10N's integrated antenna as the starting constraint.

---

## Sources

- [Core Electronics — SparkFun Dualband L1/L5 GNSS Breakout, DAN-F10N](https://core-electronics.com.au/sparkfun-dualband-l1-l5-gnss-gps-breakout-ublox-dan-f10n.html)
- [Core Electronics — SparkFun GNSS-RTK L1/L5 Breakout, NEO-F9P (Qwiic)](https://core-electronics.com.au/sparkfun-gnss-rtk-l1l5-breakout-neo-f9p-qwiic.html)
- [u-blox DAN-F10N product page](https://www.u-blox.com/en/product/dan-f10n-module)
- [Geoscience Australia — SouthPAN FAQ](https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan/southpan-frequently-asked-questions)

---

## Sourcing — updated 2026-08-28, after the SparkFun DAN-F10N went out of stock

### The DAN-F10N is still available, just not from SparkFun

**[GNSS Store (Latvia)](https://gnss.store/products/elt0469)** lists it **in stock, over 100
units, at EUR 39.99** — cheaper than SparkFun's AUD $104.95, with the **built-in active L1/L5
stacked-patch antenna**.

### SETTLED 2026-08-26 — answers direct from GNSS Store

| Question | Answer |
|---|---|
| Ship to Australia? | Yes. **OMNIVA $19, 9–18 business days** or **DHL Express $42**. |
| Which carrier? | **DHL.** Their own words: *"OMNIVA makes one delivery attempt, then sends it back."* |
| Lithium backup battery? | **No.** A **0.011 F supercapacitor** for hot start. No shipping restriction anywhere. |
| Is I2C broken out? | **No — the DAN-F10N has no I2C at all. UART only.** Their listing was wrong; they acknowledged the bug and are fixing it. |

**Decision: DHL.** A single delivery attempt followed by return-to-Latvia is a catastrophic
failure mode for a part with no local stock. The $23 premium is trivial insurance.

**The I2C question is closed, and the answer is no.** The UART migration below is required, not
optional. Confirmed against the
[DAN-F10N product summary](https://cdn.shopify.com/s/files/1/0991/6262/0236/files/DAN-F10N_ProductSummary_UBXDOC-963802114-13162.pdf):
**"1 UART"**, no I2C/DDC, no SPI.

**Supercapacitor expectation.** 0.011 F is an ephemeris/RTC backup measured in **hours**, not the
"day or two" quoted. After overnight storage expect a cold or warm start and a longer time to
first fix — not a fault, just something to allow for at the start of a shift.

### Siderion Polaris F10N (France) — checked 2026-08-26, REJECTED for EcoSync

[Product page](https://siderion.io/products/polaris-f10n-dan-f10n-gnss-and-rm3100-module).
Siderion is **Drotek Electronics rebranded** — an established French drone-electronics maker, so
this is a credible product, just not for this job.

| | |
|---|---|
| Price | **USD $96.00** (sale, from $119.00), tax excl. — roughly **double** GNSS Store's EUR 39.99 |
| Stock | **12 in stock**, ships 1–2 business days |
| GNSS | u-blox DAN-F10N, L1/L5, "As good as 1 m (CEP)", 20 Hz, NavIC, SAW-LNA-SAW anti-jam |
| Extra sensors | **RM3100 magnetometer + BMP581 barometer** |
| Interface | listed as "UART - I2C" |
| Antenna | **embedded passive patch** — correct by design; the DAN-F10N has its own LNA front end |
| Physical | 38 × 38 × 15 mm, 30 g, **6-pin JST-GH** (Pixhawk standard), one cable included |

**The "I2C" does NOT help us.** It serves the RM3100 and BMP581. The DAN-F10N itself has no I2C
pin — u-blox's product summary says "1 UART". The UART migration is still required.

**Why reject:** roughly double the price for the same GNSS receiver plus a magnetometer and a
barometer that feed nothing EcoSync records — and on a vehicle spray rig a magnetometer is worse
than useless (hard/soft-iron interference from bullbar, hose reel, pump motor). The JST-GH
connector also needs terminating to reach the ESP32.

**When it WOULD be right:** a drone-mounted survey capability. It is purpose-built for that —
MAVLink, JST-GH, autopilot integration. Worth remembering if that ever becomes a product line.

**Marketing red flag worth noting:** their headline says *"Achieve sub-meter accuracy"* while
their own spec table says *"As good as 1 m (CEP)"*. That is exactly the overclaim banned from the
StoryMap. Do not copy vendor marketing into a government pitch.

*(Siderion also stock the newer **u-blox F20P** platform — worth a look only if RTK ever gets
justified.)*

### The SAM-M10Q alternative (ELT0169) — tempting, but no

GNSS Store noted the **SAM-M10Q** sits on the same board and **does** have I2C, which would mean
zero firmware change. Per the
[SAM-M10Q product summary](https://content.u-blox.com/sites/default/files/documents/SAM-M10Q_ProductSummary_UBX-22004868.pdf):

| | Fitted now (M9) | SAM-M10Q | DAN-F10N |
|---|---|---|---|
| Accuracy | 2.0 m CEP | **1.5 m CEP** | **1.0 m CEP (with SBAS)** |
| Bands | L1 only | **L1 only** | **L1/L5/E5a/B2a** |
| SouthPAN named | no | **no** | yes (F10 platform) |
| Interface | I2C | **I2C — drop-in** | UART — migration needed |
| Antenna | — | built-in 15×15 mm patch | built-in L1/L5 patch |

**Reject it.** It buys 0.5 m over what is already fitted and gives up the entire argument the
pitch rests on: dual-band L1/L5, SouthPAN augmentation, and the "inherits FOC 2028 improvements
free, no hardware replacement" roadmap claim. Trading that narrative to avoid an hour of
firmware work is a bad trade.

**Still to verify before the SouthPAN claim goes to DEW:** the DAN-F10N *product summary* is
abbreviated and lists only "SBAS L1C/A, BDSBAS B1C" — it does not name SouthPAN. The full
**NEO-F10N datasheet** does name it explicitly, and both are F10 platform, so this is very
likely fine. Get the full DAN-F10N *datasheet* (not the product summary) and confirm before
putting SouthPAN in writing for a government client.

Downside remaining: international shipping and customs from Latvia.

### Local alternative — NEO-F10N, confirmed equivalent

The [u-blox NEO-F10N datasheet](https://content.u-blox.com/sites/default/files/documents/NEO-F10N_DataSheet_UBX-23002117.pdf)
settles it. **SBAS list: "EGNOS, GAGAN, MSAS, WAAS, BDSBAS, KASS and SouthPAN"** — SouthPAN
named explicitly. **"Position accuracy (CEP) 1 m"** with SBAS. Identical to the DAN-F10N on
both counts. Also **UART only** at module level, so the firmware change is the same either way.

| Part | Core Electronics | Stock | Price AUD |
|---|---|---|---|
| SparkFun NEO-F10N L1/L5 Breakout, SMA (GPS-24114) | [link](https://core-electronics.com.au/sparkfun-gnss-l1l5-breakout-neo-f10n-sma.html) | 6 | $72.27 |
| u-blox ANN-MB5 active L1/L5 antenna (GPS-25071) | [link](https://core-electronics.com.au/u-blox-multi-band-active-gnss-antenna-l1-l5-ann-mb5.html) | 11 | $73.55 |
| | | **Total** | **$145.82** |

### Antenna warning — one listing will silently cost you L5

**GPS L5 sits at 1176.45 MHz.** Check every antenna against that figure, because "multi-band"
is not the same as L1/L5.

| Antenna | Second band | Covers L5? | Price |
|---|---|---|---|
| SparkFun Multi-Band Magnetic Mount (GPS-15192) | **1197–1249 MHz** | **NO** — this is L1/**L2** | $219.95 |
| u-blox ANN-MB5 (GPS-25071) | 1164–1186 MHz | yes | $73.55 |
| SparkFun L1/L2/L5 Helical (GPS-23847) | L1/L2/L5 triband | yes | $229.95 |

The magnetic mount appears in searches for "L1 L5" and is described as "L2/L5", but 1176 MHz is
**below** its 1197 MHz lower limit. Fitting it would give L1-only performance, no SouthPAN DFMC
benefit, and no error message — the accuracy simply would not improve and nothing would explain
why. It is also three times the price of the antenna that does work.

### Which to buy

**It depends on where the unit physically sits, and that is your call.**

- **If the EcoSync Field stays clipped to the spray gun** and moves with the operator, an
  antenna on a 3 m cable is impractical. The **DAN-F10N with its integrated patch** is the right
  architecture, and GNSS Store is the place to get it.
- **If the unit could live in the cab** with only the trigger on the gun, then **NEO-F10N +
  ANN-MB5 on the roof or bullbar** will outperform any integrated patch, because a clear sky
  view matters more than the module. Local stock, this week.
- **For the Micro**, the integrated patch is the only sensible answer regardless — nothing else
  suits a belt-worn device.

### Lesson for the pitch copy

Naming a specific part number in a published document makes you a hostage to stock. You have
just experienced that. Change the StoryMap from *"(u-blox DAN-F10N class)"* to
*"the u-blox F10 dual-band (L1/L5) platform"* — accurate for both modules, both give 1 m CEP
with SouthPAN, and it survives the next supply hiccup without another republish.
