# GNSS hardware decision — settled, and validated in the field

Researched 26 Aug 2026. **Decision closed 11 Sep 2026** against three days of paired field
measurement. The F10 platform was chosen on specification; it has now been confirmed on the
gun, in scrub, against the device it replaces.

---

## Status

| | |
|---|---|
| Module fitted | **u-blox DAN-F10N**, PROTVER 40.0, L1/L5 dual-band |
| Augmentation | **SouthPAN, confirmed working** — corrections observed being applied |
| Interface | UART1 @ 38400, with automatic I²C fallback in firmware |
| Measured accuracy | **1.82 m** mean over 24 paired readings, all under SouthPAN-capable sky |
| Against iPhone | **5.10 m** over the same 24 readings. **24 wins from 24.** |
| RTK | Not required, not fitted, on the roadmap only |

The one open hardware question is the **external antenna**, covered at the end.

---

## What was actually measured

Three field days, EcoSync against an iPhone, side by side, same moment, same place.

| | 26 Aug | 10 Sep | 11 Sep |
|---|---|---|---|
| Receiver | M9 (L1 only) | F10 (L1/L5) | F10 (L1/L5) |
| Mount | gun handle | gun handle, antenna facing down | side of handle |
| EcoSync mean | 2.82 m | 1.88 m | 1.77 m |
| EcoSync sd | 0.38 | 0.59 | 0.47 |
| iPhone mean | 5.47 m | 5.24 m | 4.96 m |
| iPhone sd | 1.25 | 1.42 | 1.37 |
| EcoSync wins | 11 / 12 | 12 / 12 | 12 / 12 |
| Advantage | 1.94× | 2.79× | 2.81× |

**Pooled across all three days (n = 36): EcoSync 2.16 m, iPhone 5.22 m, 35 wins from 36.**

**F10 days only (n = 24): EcoSync 1.82 m, iPhone 5.10 m, 24 wins from 24.**

### What each change was worth

| Change | Measured effect | Confidence |
|---|---|---|
| M9 → F10 with SouthPAN | **−1.00 m** (2.82 → 1.82) | Large, consistent, two independent days |
| SouthPAN on vs off | **−0.36 to −0.57 m** | Replicated on both F10 days, same direction, near-identical spreads |
| Antenna orientation on the gun | **no measurable effect** | diff 0.11 m, t = 0.52, df = 21 — indistinguishable from noise |
| Power configuration | 12.1 → **6.4 %/h**, ~14 h runtime | Single full working day, 8:30 to 16:00 |

The M9 → F10 improvement of almost exactly one metre is the number that justifies the module
change. It was predicted from specification (2.0 m CEP → 1.0 m CEP with SBAS) and it landed
within noise of the prediction.

### The finding that matters most commercially

From the 11 Sep canopy breakdown:

| Canopy | EcoSync | iPhone | Gap |
|---|---|---|---|
| Open sky | 1.37 m | 2.80 m | 1.43 m |
| Light | 1.44 m | 5.00 m | 3.56 m |
| Medium | 2.02 m | 5.55 m | 3.52 m |
| Heavy | 2.16 m | 5.70 m | 3.54 m |

**Open sky to heavy canopy, the iPhone degrades by 2.90 m. EcoSync degrades by 0.79 m.**
It holds up **3.7× better** in exactly the conditions the work happens in.

This is the argument to lead with, and it is stronger than any single accuracy figure, because
it answers the objection a board will actually raise: *we already have phones.* Under a
boxthorn in a gully, a phone is a materially different instrument than it is in the depot car
park. A dedicated receiver is not.

---

## The SouthPAN trap — record this, it cost a week

SouthPAN is still broadcasting with the **SBAS test-mode flag set (SBAS message type 0)**.
u-blox receivers **discard corrections from a test-mode SBAS provider by default.**

The failure mode is silent and extremely misleading: the receiver acquires the satellite,
tracks it, reports it in the satellite list — and then bins every correction. The symptom
looks exactly like intermittent SBAS caused by poor sky view, which sends you chasing antenna
siting and satellite geometry. It is not a geometry problem and no amount of antenna work will
fix it.

The fix is one configuration key, per Geoscience Australia's own receiver documentation:

```cpp
myGNSS.setVal8(UBLOX_CFG_SBAS_USE_TESTMODE, 1);   // the one that matters
```

Set alongside `SIGNAL_SBAS_ENA`, `SIGNAL_SBAS_L1CA_ENA`, `SBAS_USE_DIFFCORR`, and
`SBAS_PRNSCANMASK` = 4 (PRN 122, Inmarsat-4F1). Integrity and ranging are left off. The
firmware records whether every key was accepted and prints the result at boot, so a silent
refusal can never masquerade as a working configuration again.

**Verification that it is genuinely working:** the firmware polls `getDiffSoln()` and reports
whether a differential solution is actually in use at that moment — not merely whether an SBAS
satellite is visible. Every reading tagged "SBAS" in the datasets above carries that live flag.

### SouthPAN behaviour observed in the field

**It acquires slowly and then holds.** On 11 Sep the first four readings were unaugmented and
every reading thereafter was augmented, including two under heavy canopy. Two of the four
*unaugmented* readings were also heavy canopy — so the early failures were the warm-up period,
not the vegetation.

The mechanism: the receiver must download a full SBAS message set including the ionospheric
grid, which cycles over several minutes. Once held, those corrections carry defined validity
windows, so brief blockages are ridden through rather than invalidating the solution.

From the Eyre Peninsula, Inmarsat-4F1 sits at roughly **50° elevation, just east of due
north** — high enough that mallee and low scrub rarely cut the line.

**This converts into an operating procedure worth more than a hardware change:**

> Switch on in the open at the vehicle. Do not begin logging until the dashboard shows SBAS.

Those four unaugmented readings were pure warm-up loss. At 0.36–0.57 m, avoiding them is a free
accuracy gain requiring no hardware, no cost and no firmware change.

---

## Why the F10, and why not RTK

**Without RTK corrections the expensive module is worse.**

| Module | Quoted CEP (50%) | ≈ 95% confidence | Price AUD | Antenna |
|---|---|---|---|---|
| DAN-F10N **with SBAS** | **1.0 m** | ~2.1 m | ~$105 | patch included |
| DAN-F10N without SBAS | 1.5 m | ~3.1 m | — | — |
| NEO-F9P **without RTK** | 1.5 m | ~3.1 m | $439.95 | **not included** |
| NEO-F9P **with RTK** | 0.01 m | — | $439.95 | not included |

Because SouthPAN *is* SBAS, the F10 runs in its best mode for free, everywhere, with no data
link. The F9P only beats it once you are actively feeding it RTCM — and until then it sits half
a metre worse, for roughly four times the price, before you have bought an antenna.

### RTK needs three things this project does not have

1. **An NTRIP client in firmware** — the device must pull RTCM from a caster and push it into
   the module continuously.
2. **A live data link in the paddock.** EcoSync only reaches Wi-Fi at sync time. RTK needs
   corrections *while* you are spraying — a cellular modem or a tethered phone.
3. **A CORS or network RTK subscription**, an ongoing per-unit cost.

Miss any one and the F9P is a worse receiver that cost four times as much. Buy it only when a
specific job justifies centimetres and all three pieces are in place.

### Cost across a fleet

At roughly $105 versus $550-plus per unit installed, this is the difference between a viable
quote to a landscape board and an unviable one. For the **EcoSync Micro**, the F10's integrated
20 × 20 × 8 mm dual-band patch is close to decisive on footprint alone.

---

## Claim guidance — revised 11 Sep 2026

**The previous warning against claiming "approximately one metre" is withdrawn.** It was
correct for the M9 and is wrong for the hardware now fitted. The F10 data supports metre-level,
dual-band L1/L5 and SouthPAN augmentation.

**What you may now write:**

- "Approximately one metre horizontal accuracy (1.0 m CEP)" — specification, and consistent with
  1.82 m measured under working conditions including heavy canopy
- "Dual-band L1/L5"
- "SouthPAN augmented" — measured, replicated, with the differential solution confirmed live
- "Metre-level"

**What still must not be written:**

- **"Sub-metre."** CEP is the 50th percentile; GIS and survey specifications are normally quoted
  at 95%. A 1.0 m CEP is roughly **2.1 m at 95% confidence**, and a GIS officer will read it as
  a 95% figure. Independent SouthPAN testing did describe the L1 service as sub-metre, but that
  is an observation about the service, not a specification you can hold this receiver to.
- **"Survey-grade."** It implies centimetres.
- **A specific part number in published copy.** Write "the u-blox F10 dual-band (L1/L5)
  platform" — accurate for both DAN-F10N and NEO-F10N, and it survives the next supply hiccup
  without a republish.

**Lead with the comparison, not the figure.** "Two-and-a-half to three metres better than the
phone you are using today, in the scrub where you actually work" is a claim from a side-by-side
test. "One metre" invites an argument about CEP versus 95% that you gain nothing from having.

**And remember what is actually being sold.** Not centimetres — the difference between 3–8 m and
about a metre. Which is exactly the difference between *"a weed was sprayed somewhere in this
paddock"* and *"this individual boxthorn was treated, and here it is again next season."* That
argument needs no RTK, and overclaiming would put every other figure in the document under
suspicion.

### The caveat to raise before it is raised for you

Every figure in this document is **the receiver's own estimate of its own error**, compared
against **the handset's estimate of its own error**. Neither has been measured against a
surveyed mark.

Handset figures are known to be optimistic because the phone fuses Wi-Fi, cell and motion data
into its estimate, so the true gap is more likely wider than narrower. But a GIS analyst is
precisely the person who spots self-reported accuracy, and it is far better raised by you than
from the floor.

**The fix is cheap:** occupy a known point repeatedly — a survey mark, or a feature that can be
identified unambiguously on high-resolution imagery — and measure actual displacement and
scatter. One afternoon converts the whole dataset from *the device grading its own homework* to
*we checked it against a mark*. Worth more than another twelve paired readings.

---

## Open item — external antenna

**Settled by measurement:** orientation of the integrated patch on the gun does not matter.
Moving it from facing the ground to facing sideways-up produced a 0.11 m change, statistically
indistinguishable from noise (t = 0.52). SparkFun describe the module's patch as having "wide
beamwidth" that "provides flexibility in the device's orientation" — the null result is
consistent with the design intent. Canopy attenuation dominates the error budget; orientation
does not.

**Still open:** whether splitting the antenna off the unit entirely improves things. The value
is not primarily accuracy — it is that a body-worn or belt-worn configuration stops needing the
enclosure to have a sky view at all, which removes the mounting problem for both the Field unit
in point-logging mode and the **Micro**.

Two routes, priced 11 Sep 2026:

| Route | Cost AUD | Notes |
|---|---|---|
| **A.** ANN-MB5 on the existing DAN-F10N via U.FL | ~$85 | Cheapest; carries real unknowns |
| **B.** Second receiver: NEO-F10N SMA + ANN-MB5 | ~$153 | Purpose-built, and enables paired testing |

**Route A risks.** SparkFun's DAN-F10N documentation describes the U.FL connector and the
`EXT_ANT` jumper but **says nothing anywhere about antenna bias voltage**. The ANN-MB5 is an
*active* antenna — it needs DC fed up the coax. If the board does not supply it, the antenna
does not work at all. Before committing: cut `EXT_ANT`, then measure DC between the U.FL centre
pin and its shell. ~3.3 V and it will work; 0 V and a bias tee is required. Note also that
cutting `EXT_ANT` **disables the integrated patch** (re-bridgeable with solder, but not between
readings), and U.FL connectors are rated for roughly 30 mating cycles.

**Route B is preferred.** The [NEO-F10N SMA breakout](https://core-electronics.com.au/sparkfun-gnss-l1l5-breakout-neo-f10n-sma.html)
($79.50) explicitly feeds bias and lists the ANN-MB5 as its recommended antenna, so the unknown
disappears. More importantly it leaves the working unit untouched, which means the integrated
patch and the external antenna can be run **simultaneously, same sky, same minute** — a true
paired comparison. Given that the orientation test came back statistically null at n = 12,
sequential testing is demonstrably not sharp enough to resolve differences of this size.
It also provides the second receiver the Micro needs. Same F10 generation, same PROTVER 40.0,
same library — **no firmware changes required**.

### Antenna warning — one listing will silently cost you L5

**GPS L5 sits at 1176.45 MHz.** Check every antenna against that figure, because "multi-band"
is not the same as L1/L5.

| Antenna | Second band | Covers L5? | Price AUD |
|---|---|---|---|
| SparkFun Multi-Band Magnetic Mount (GPS-15192) | **1197–1249 MHz** | **NO** — this is L1/**L2** | $219.95 |
| [u-blox ANN-MB5 (GPS-25071)](https://core-electronics.com.au/u-blox-multi-band-active-gnss-antenna-l1-l5-ann-mb5.html) | 1164–1186 MHz | **yes** | $73.55 |
| SparkFun L1/L2/L5 Helical (GPS-23847) | L1/L2/L5 triband | yes | $229.95 |

The magnetic mount appears in searches for "L1 L5" and is described as "L2/L5", but 1176 MHz is
**below** its 1197 MHz lower limit. Fitting it would give L1-only performance, no SouthPAN DFMC
benefit, and no error message — the accuracy simply would not improve and nothing would explain
why. It is also three times the price of the antenna that does work.

**Do not buy a GNSS antenna over the counter.** Jaycar and similar carry L1-only GPS antennas at
1575 MHz. One would work, look like a fix, and silently cost L5 and most of the F10 gain.

ANN-MB5 specifics: L1 1559–1608 MHz, L5/E5a/B2a/NavIC **1164–1186 MHz**, peak gain 4.5 / 4.0
dBic, 49 × 46 × 16 mm, 104 g including cable, 3 m RG174, SMA, magnetic base. The magnetic base
needs something ferrous — there is none on a spray gun or a belt, so a steel plate or a printed
cradle is required. Do not extend the cable; 3 m of RG174 is already about 3 dB at L-band.

**For the Micro, longer term:** Taoglas make 20 × 20 mm and 25 × 25 mm active L1/L5 stacked
patches with U.FL pigtails fitted — the right class of part for a wearable. Distributor items
through Mouser AU, longer lead time.

### Which configuration suits which deployment

The antenna question is really a question about where the unit physically sits, and that has
three different answers:

- **Field unit clipped to the spray gun**, moving with the operator. An antenna on a 3 m cable is
  impractical. The **integrated patch is the right architecture** — and the measured 1.82 m says
  it is good enough. This is the configuration in service.
- **Unit in the cab**, with only the trigger and pad on the gun. Then **an external antenna on
  the roof or bullbar will outperform any integrated patch**, because an uninterrupted sky view
  matters more than the module does. Worth considering for permanent vehicle fitment, where the
  cable is not a nuisance and the unit can be hard-wired for power.
- **Body-worn — the Micro, and the Field unit in point-logging mode.** The integrated patch is
  workable but the operator's own torso blocks roughly half the sky at belt height. An antenna on
  a shoulder strap or pack top is the better answer, and it is why route B above is worth the
  extra $80.

---

## ESP32 board — no change needed

The SparkFun Thing Plus ESP32 WROOM stays. It has spare UARTs for the F10, SPI for the SD card
and I²C for the fuel gauge, and the firmware uses a fraction of its flash and RAM. Changing to
an S3 would mean reworking pin assignments and re-testing everything for no benefit currently
nameable.

The one place a different board earns its keep is the **EcoSync Micro**, where the Thing Plus
footprint is the limiting factor for a wearable. That is a fresh enclosure and power design
rather than an upgrade to the Field unit — its own exercise, with the F10's integrated antenna
as the starting constraint.

### Firmware: UART, not I²C

The DAN-F10N has **no I²C at all** — u-blox's product summary says "1 UART". The migration is
complete and in service:

```cpp
Serial1.begin(38400, SERIAL_8N1, 16, 17);   // 16 = breakout TXD, 17 = breakout RXD
bool gnssOk = myGNSS.begin(Serial1);        // falls back to I2C if UART does not answer
```

Everything downstream — `pollGPS()`, the caching, `gpsUsable()` — was unchanged, because it only
reads from the library. A side benefit: it freed the I²C bus, leaving only the battery gauge.

---

## Appendix — sourcing history, Aug 2026

*Retained as record. The procurement question is closed; this is why.*

### GNSS Store (Latvia) — the chosen supplier

**[GNSS Store](https://gnss.store/products/elt0469)** listed the DAN-F10N in stock at **EUR
39.99**, cheaper than SparkFun's AUD $104.95, with the built-in active L1/L5 stacked-patch
antenna. Answers direct from them, 26 Aug:

| Question | Answer |
|---|---|
| Ship to Australia? | Yes. **OMNIVA $19, 9–18 business days** or **DHL Express $42**. |
| Which carrier? | **DHL.** Their own words: *"OMNIVA makes one delivery attempt, then sends it back."* |
| Lithium backup battery? | **No.** A **0.011 F supercapacitor** for hot start. No shipping restriction. |
| Is I²C broken out? | **No — the DAN-F10N has no I²C at all.** Their listing was wrong; they acknowledged the bug. |

**Supercapacitor expectation:** 0.011 F is an ephemeris/RTC backup measured in **hours**, not the
"day or two" quoted. After overnight storage expect a warm or cold start and a longer time to
first fix — not a fault, just something to allow for at the start of a shift. Combined with the
SBAS warm-up above, this reinforces the "power on at the vehicle" procedure.

### Rejected alternatives

**Siderion Polaris F10N (France)** — [product page](https://siderion.io/products/polaris-f10n-dan-f10n-gnss-and-rm3100-module).
Drotek Electronics rebranded; credible product, wrong job. USD $96 (roughly double GNSS Store)
for the same GNSS receiver plus an RM3100 magnetometer and BMP581 barometer that feed nothing
EcoSync records — and on a vehicle spray rig a magnetometer is worse than useless (hard and
soft-iron interference from bullbar, hose reel, pump motor). Its "I2C" serves the extra sensors,
not the GNSS module, so the UART migration was required anyway. The JST-GH connector also needs
terminating. *Would be right for a drone-mounted survey capability — MAVLink, JST-GH, autopilot
integration — if that ever becomes a product line.*

*Marketing red flag worth noting:* their headline says "Achieve sub-meter accuracy" while their
own spec table says "As good as 1 m (CEP)". Exactly the overclaim banned above. Do not copy
vendor marketing into a government pitch.

**SAM-M10Q (ELT0169)** — tempting because it has I²C and would have meant zero firmware change.
Rejected: it buys 0.5 m over the M9 already fitted (1.5 m CEP vs 2.0 m) and gives up the entire
argument the pitch rests on — dual-band L1/L5, SouthPAN augmentation, and the "inherits FOC 2028
improvements free, no hardware replacement" roadmap claim. Trading that narrative to avoid an
hour of firmware work was a bad trade, and the measured 1.00 m improvement from the F10
confirms it.

### The SouthPAN documentation question — closed

The DAN-F10N *product summary* is abbreviated and lists only "SBAS L1C/A, BDSBAS B1C" without
naming SouthPAN, which left a documentation gap before purchase. The
[NEO-F10N datasheet](https://content.u-blox.com/sites/default/files/documents/NEO-F10N_DataSheet_UBX-23002117.pdf)
names it explicitly — **"EGNOS, GAGAN, MSAS, WAAS, BDSBAS, KASS and SouthPAN"**, with
**"Position accuracy (CEP) 1 m"** — and both are F10 platform.

**That question is now closed empirically rather than on paper.** SouthPAN corrections have been
observed being applied by the fitted DAN-F10N, with a measured benefit replicated across two
field days. The claim can go to DEW.

### Local alternative — NEO-F10N, confirmed equivalent

Identical on both counts that matter, and UART-only at module level so the firmware is the same
either way.

| Part | Core Electronics | Price AUD |
|---|---|---|
| [SparkFun NEO-F10N L1/L5 Breakout, SMA (GPS-24114)](https://core-electronics.com.au/sparkfun-gnss-l1l5-breakout-neo-f10n-sma.html) | link | $79.50 |
| [u-blox ANN-MB5 active L1/L5 antenna (GPS-25071)](https://core-electronics.com.au/u-blox-multi-band-active-gnss-antenna-l1-l5-ann-mb5.html) | link | $73.55 |
| | **Total** | **$153.05** |

---

## Sources

- [Core Electronics — SparkFun Dualband L1/L5 GNSS Breakout, DAN-F10N](https://core-electronics.com.au/sparkfun-dualband-l1-l5-gnss-gps-breakout-ublox-dan-f10n.html)
- [Core Electronics — SparkFun GNSS-RTK L1/L5 Breakout, NEO-F9P (Qwiic)](https://core-electronics.com.au/sparkfun-gnss-rtk-l1l5-breakout-neo-f9p-qwiic.html)
- [SparkFun DAN-F10N hookup guide — hardware overview](https://docs.sparkfun.com/SparkFun_GNSS_DAN-F10N/hardware_overview/)
- [u-blox DAN-F10N product page](https://www.u-blox.com/en/product/dan-f10n-module)
- [u-blox NEO-F10N datasheet](https://content.u-blox.com/sites/default/files/documents/NEO-F10N_DataSheet_UBX-23002117.pdf)
- [u-blox ANN-MB5 antenna](https://www.u-blox.com/en/product/ann-mb5-antenna)
- [Geoscience Australia — SouthPAN FAQ](https://www.ga.gov.au/scientific-topics/positioning-navigation/positioning-australia/about-the-program/southpan/southpan-frequently-asked-questions)
- [Geoscience Australia — SouthPAN GNSS Receiver Documentation](https://geoscienceaustralia.github.io/SouthPAN-GNSS-Receiver-Documentation/)
