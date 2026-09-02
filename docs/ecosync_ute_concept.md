# EcoSync Ute — vehicle-mounted variant

Concept raised by James, 26 Aug 2026. Revised the same day once the working distance was known.

---

## VERDICT: a roof antenna cannot be the position source. But there is still a Ute product.

**The measured answer that decides it: the hose reels are 100 m, and the operator is routinely
100–200 m from the vehicle.**

Against a 2 m accuracy target, a 100–200 m offset is not a trade-off to be managed. It is a
50–100× error. Every gain from a roof-mounted antenna, every argument for L5, every decimal place
in the CEP figure — all of it is swamped by recording the ute's position instead of the plant's.

**So: do not build a Ute unit that logs spray position from a vehicle-mounted antenna.** The
receiver has to be where the spray is. That is the existing EcoSync Field, and it is already the
right answer.

This kills the antenna premise. It does **not** kill the vehicle unit.

---

## What a Ute unit should actually be: power and connectivity, not positioning

Every real problem the vehicle can solve is a power or comms problem, and those are problems you
have actually hit:

**1. Battery drain.** Running at 14 dBm noticeably shortened runtime, and streaming NMEA to a
phone keeps Wi-Fi awake continuously. A vehicle-powered unit has no such constraint, and can serve
as a **charging dock** for Field and Micro units between jobs.

**2. Sync only happens in Wi-Fi range.** Today the Field unit holds records on the SD card until it
reaches Starlink. A Ute unit on vehicle 12 V, running a permanent access point with a proper
external Wi-Fi antenna and a Starlink or 4G uplink, becomes a **backhaul hub** — the Field unit
syncs to it from the paddock, and it forwards to ESRI. Records reach the board the same day
instead of the same week.

**3. It is the natural home for a screen or status display**, if the platform ever wants one.
The cab is where an operator can actually look at something.

**Recommended positioning: "EcoSync Ute — the vehicle hub."** Power, connectivity and sync for the
units that do the measuring. Sold as a companion to Field and Micro, not as a third way to log a
point.

### The one thing to test before committing to the hub idea

**Can the Field unit reach the Ute unit's access point at 100–200 m through scrub?** That is the
whole premise. ESP32 at 14 dBm to a well-sited external antenna is plausible at 100 m line of
sight, and marginal-to-hopeless through dense vegetation. Test it before designing around it. If
the range does not hold, the hub still works as a **charging dock and sync point the operator
returns to**, which is less elegant but still useful.

---

## Where a vehicle-mounted antenna WOULD be right

Only when the vehicle genuinely is the spray point:

| Spraying mode | Right unit |
|---|---|
| Boom spraying / roadside spraying from a moving vehicle | Vehicle-mounted antenna is correct |
| Spraying from the tray, within a few metres | Either works |
| **Hose reel out to individual plants (100–200 m)** | **Field, at the gun. Nothing else.** |
| Foot work away from the vehicle | Micro |

If roadside or boom spraying ever becomes part of the business, revisit. It is a different job,
not a better version of this one. At vehicle speed `setNavigationFrequency(1)` would also need
raising — at 30 km/h that is 8.3 m between fixes.

---

## Measured result, 26 Aug 2026

Spray gun resting on the back of the ute, EcoSync Field attached and logging, connected to
QuickCapture, operator close to the vehicle, cut-and-swab work:

**Reported accuracy ≈ 1.6 m.**

Better than the NEO-M9N datasheet's 2.0 m CEP, which is consistent with good sky view and a static
receiver. Worth recording as the realistic best case for the current L1-only hardware.

---

## Withdrawn: two pieces of homework that no longer apply

Both existed only to solve "the unit is in the cab, the trigger is at the gun". That design is
dead, so neither is needed.

**~~Check the hose reel for a spare core or slip ring.~~** *(A slip ring is an electrical connector
that carries current across a rotating joint, so a wire can run onto a spinning reel without
twisting off. A spare core is an unused conductor in a cable already routed through. Both were
ways to get a trigger signal from the gun back to a cab-mounted unit — unnecessary now the unit
stays at the gun.)*

**~~Price a pressure switch.~~** It was an alternative to running that wire: detect spraying
hydraulically at the pump instead of electrically at the gun. Also unnecessary.

**One residual thought, entirely optional.** A flow or pressure switch detects *product actually
leaving the nozzle*, where a trigger detects *a switch being squeezed*. Today, a squeeze with the
pump off or the tank empty still writes a record. If false records ever become a data-quality
complaint, that is the fix. It is not a reason to change anything now.
