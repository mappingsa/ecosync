# EP East delivery — prep checklist
**Internal. Monday prep for Tuesday delivery and field trial.**

The unit is going to misbehave in front of a client or it isn't, and that is decided on
Monday.

---

## Before Tuesday — the non-negotiables

- [ ] **Flash `20261003-a uart buffer`** to the trial unit. This is not yet on either unit,
      and it is the fix for the field data loss. Do not send a unit out without it.
- [ ] **If the trial unit is 002** — reflow the GNSS joints first (TXD→GPIO16, then GND,
      then 3V3), and pull `/rejected.csv` off the card before you do, so the forensic record
      survives. If there is any doubt about 002, **send 001**. It logged 1,450 clean points.
- [ ] **Put `gps_max_acc` back to 7.0** if it is still at 10.0.
- [ ] **Bench-test the new touch pad** when it arrives Monday. The AER-01 taught this lesson
      already — an untested pad in front of a client is an avoidable risk. Press it 100
      times on the bench and confirm 100 records.
- [ ] **Charge to full** Monday night.

## Dashboard will be on show — set it up for that

- [ ] **Set the access point to stay on** (`ap_always`) for the demo. The default idle
      shutdown after 10 minutes with a 120 s window every 15 is correct for field power
      saving and **wrong for a day when people keep wanting to look at the dashboard**. The
      AP disappearing mid-demo is a bad look for a system you are selling on simplicity.
      Set it back before you leave it with them, and tell them you have.
- [ ] Confirm the dashboard loads on **a phone you did not configure** — theirs, ideally.
- [ ] Check the unit ID shown on the dashboard is the one you intend them to have.

## SD card

- [ ] `config.txt` written for this unit and this Board
- [ ] `ca.pem` present
- [ ] Card blank of previous field data so their trial starts clean
- [ ] Spare card in the bag

---

## Ask EP East's GIS contact **before** Tuesday

A two-line email today or tomorrow is worth more than an hour of demonstration:

> Before I come over Tuesday — can you send me the feature class definition you'd want weed
> records in (field names, types, and any coded-value domains, especially the species list),
> and confirm whether your data is GDA94 or GDA2020?

If that arrives before Tuesday, **the points captured in the morning can be in their
structure by the afternoon**. That single demonstration answers their number one concern
better than anything you can say about it.

If it's GDA94, remember there's roughly a 2 m systematic shift from WGS84 that has to be
transformed out — bigger than the unit's own accuracy.

---

## The day itself

**Three things they said matter. Demonstrate each one, don't describe it.**

### Data flow
Capture in the morning. Have it visible in a layer they can open that afternoon. If the
schema arrived in time, in their schema. Nothing else you do will land as hard.

### Accuracy
Don't quote the spec sheet — prove it in front of them:

- Log a point on a **known mark** and show the offset
- Log the **same spot several times** and show the scatter
- Have someone **hold their phone at the same point** and compare. You have 35 of 36 on
  record; reproducing it live in front of them is worth more than the table.
- Show them the **per-record accuracy value** in the data. "You can filter on this" is the
  thing a GIS person will care about most.

### Simplicity
**Hand it to their field officer and say nothing.** Let them work it out. If it is as simple
as you believe it is, that silence is the most persuasive thing that happens all day — and
if it isn't, you have just learned something worth far more than a smooth demo.

---

## Bring

- [ ] Both sheets (EcoSyncField/Micro, EcoSyncID) printed
- [ ] Trial scope, two copies, for signature
- [ ] Charger, spare SD card, spare pad if you have one
- [ ] Laptop, for showing the data landing

---

## Don't

- Don't leave without the GIS contact's name, email and the datum answer.
- Don't quote a unit price. Range and a written quote to follow.
- Don't commit to a date for Micro or ID.
- Don't leave the unit without telling them plainly: **charge it every night.**
