// =====================================================================
//  MSA EcoSync Field - 3D printed enclosure, parametric draft v1
//
//  OpenSCAD (free, openscad.org). Open this file, press F5 to preview,
//  F6 to render, then File > Export > STL.
//
//  EVERY dimension is a named variable below. Measure your actual parts
//  and correct them before printing anything you intend to keep - the
//  component sizes are from SparkFun's published specs, not from calipers
//  on YOUR boards, and headers and solder blobs always add a millimetre.
//
//  PRINT NOTES
//    Material : ASA for field units (UV stable, ~100C).
//               PETG for fit-check prototypes.
//               NEVER PLA - it sags in a ute in an SA summer.
//               NEVER carbon-filled - it attenuates the GNSS patch.
//    Walls    : 5 perimeters minimum. Water wicks through layer lines,
//               and perimeters are what stop it.
//    Layers   : 0.2mm. Base printed open-side UP so the gasket face is a
//               top surface and comes out flat. Lid printed face down.
//    Infill   : 40% gyroid.
//  =====================================================================

$fn = 64;

// ---------- WHAT GOES INSIDE ----------------------------------------
board_l      = 64.8;   // SparkFun Thing Plus ESP32 WROOM
board_w      = 22.9;
board_clear  = 12.0;   // headroom above the PCB for headers and cables

gnss_l       = 40.6;   // SparkFun DAN-F10N breakout
gnss_w       = 40.6;
gnss_clear   = 10.0;   // 8mm patch + a little

batt_l       = 68.8;   // SparkFun 2000mAh LiPo
batt_w       = 49.2;
batt_h       = 5.6;
batt_squash  = 1.5;    // pouch cells swell with age. Leave them room.

// ---------- SHELL ----------------------------------------------------
wall         = 3.0;    // 3mm. Do not go thinner if you want it watertight.
floor_t      = 3.0;
lid_t        = 3.0;

// Interior derived from the battery footprint, which is the largest part.
inner_l      = batt_l + 6;
inner_w      = batt_w + 8;
inner_h      = batt_h + batt_squash + board_clear + gnss_clear + 6;

outer_l      = inner_l + 2*wall;
outer_w      = inner_w + 2*wall;
outer_h      = inner_h + floor_t;

// ---------- GASKET ---------------------------------------------------
// 2mm silicone O-ring CORD (buy by the metre, superglue the ends into a
// loop). Groove sized for ~25% compression, which is where a static seal
// wants to live: enough to seal, not so much it takes a set.
gasket_cord  = 2.0;
groove_w     = gasket_cord * 1.2;    // 2.4mm
groove_d     = gasket_cord * 0.75;   // 1.5mm -> 0.5mm proud -> 25% squeeze
groove_inset = wall/2;               // centred in the wall

// ---------- FASTENERS ------------------------------------------------
// M3 brass heat-set inserts, NOT screws into bare plastic. This lid will
// be opened many times over the unit's life and tapped plastic strips.
insert_d     = 4.2;    // typical M3 short insert; check yours
insert_depth = 6.0;
screw_clear  = 3.4;    // M3 clearance in the lid
screw_head   = 6.2;    // countersink / counterbore
boss_d       = insert_d + 2*2.2;
// Boss centre inset. Must be far enough in that the boss does not eat into
// the gasket groove - at 7.0 the 8.6mm boss just touched it, which would
// have left a leak path straight down the screw hole.
corner_in    = 8.5;

// ---------- PENETRATIONS ---------------------------------------------
// Both of these are the ONLY places water should be able to get in, and
// both are solved by proper hardware rather than by printing.
gland_d      = 12.5;   // M12 cable gland for the trigger lead
usb_d        = 16.0;   // waterproof panel-mount USB-C, capped
// Pressure equalisation. A sealed box in the sun pressurises, then cools
// and SUCKS water back past the seal. This is how sealed boxes actually
// fail. A Gore-style vent membrane fixes it for a couple of dollars.
vent_d       = 12.5;

// ---------- MOUNT ----------------------------------------------------
// A flat bonded pad for the adhesive GoPro mount you are already using.
// Printing GoPro prongs to mate with commercial parts needs tolerances
// you should not fight on a first revision - bond the real one instead.
pad_l        = 44;
pad_w        = 34;
pad_recess   = 0.6;    // shallow recess so the pad cannot creep

// =====================================================================
//  BASE
// =====================================================================
module base() {
  difference() {
    union() {
      // shell
      difference() {
        rounded_box(outer_l, outer_w, outer_h, 4);
        translate([wall, wall, floor_t])
          rounded_box(inner_l, inner_w, inner_h + 1, 2);
      }
      // corner bosses for the heat-set inserts
      for (x = [corner_in, outer_l - corner_in])
        for (y = [corner_in, outer_w - corner_in])
          translate([x, y, floor_t])
            cylinder(d = boss_d, h = inner_h);

      // battery pocket walls - a shallow fence so the pouch cannot slide
      translate([wall + 2, wall + 2, floor_t])
        difference() {
          cube([batt_l + 2, batt_w + 2, 3]);
          translate([1, 1, -0.5]) cube([batt_l, batt_w, 4]);
        }

      // board standoffs, above the battery
      board_z = floor_t + batt_h + batt_squash + 2;
      translate([wall + 4, wall + 4, 0])
        standoffs(board_l, board_w, board_z);

      // GNSS standoffs, at the top, patch facing the lid
      gnss_z = board_z + board_clear;
      translate([wall + 6, wall + 6, 0])
        standoffs(gnss_l, gnss_w, gnss_z);
    }

    // gasket groove in the top rim
    translate([0, 0, outer_h - groove_d])
      groove_ring();

    // insert holes
    for (x = [corner_in, outer_l - corner_in])
      for (y = [corner_in, outer_w - corner_in])
        translate([x, y, outer_h - insert_depth])
          cylinder(d = insert_d, h = insert_depth + 1);

    // penetrations - all on ONE end, so cables leave together
    translate([-1, outer_w/2, floor_t + 14])
      rotate([0, 90, 0]) cylinder(d = gland_d, h = wall + 2);
    translate([outer_l - wall - 1, outer_w/2 - 12, floor_t + 14])
      rotate([0, 90, 0]) cylinder(d = usb_d, h = wall + 2);
    translate([outer_l - wall - 1, outer_w/2 + 14, floor_t + 14])
      rotate([0, 90, 0]) cylinder(d = vent_d, h = wall + 2);

    // GoPro adhesive pad recess on the underside
    translate([outer_l/2 - pad_l/2, outer_w/2 - pad_w/2, -0.01])
      rounded_box(pad_l, pad_w, pad_recess, 4);
  }
}

// =====================================================================
//  LID
//  The patch antenna sits directly under this. Keep it thin, keep it
//  flat, and keep metal and carbon-filled filament away from it.
// =====================================================================
module lid() {
  antenna_win = 30;    // thinned region over the 20x20 patch

  difference() {
    union() {
      rounded_box(outer_l, outer_w, lid_t, 4);
      // tongue that locates into the groove
      translate([0, 0, lid_t])
        tongue_ring();
    }
    // thin the roof over the patch. 1.2mm of ASA is effectively invisible
    // to L1/L5; 3mm of anything is still fine, but thinner is better and
    // it doubles as a visual "this way up" cue.
    translate([outer_l/2 - antenna_win/2, outer_w/2 - antenna_win/2, lid_t - 1.8])
      rounded_box(antenna_win, antenna_win, 2, 3);

    // screw holes, counterbored
    for (x = [corner_in, outer_l - corner_in])
      for (y = [corner_in, outer_w - corner_in])
        translate([x, y, -0.01]) {
          cylinder(d = screw_clear, h = lid_t + 4);
          cylinder(d = screw_head, h = 1.8);
        }
  }
}

// =====================================================================
//  HELPERS
// =====================================================================
module rounded_box(l, w, h, r) {
  hull()
    for (x = [r, l - r])
      for (y = [r, w - r])
        translate([x, y, 0]) cylinder(r = r, h = h);
}

module groove_ring() {
  difference() {
    offset_ring(groove_inset - groove_w/2, groove_w, groove_d + 1);
  }
}

module tongue_ring() {
  // slightly undersized so it drops in without forcing the gasket sideways
  offset_ring(groove_inset - (groove_w - 0.4)/2, groove_w - 0.4, groove_d - 0.5);
}

module offset_ring(inset, width, h) {
  difference() {
    translate([inset, inset, 0])
      rounded_box(outer_l - 2*inset, outer_w - 2*inset, h, 4);
    translate([inset + width, inset + width, -0.5])
      rounded_box(outer_l - 2*(inset + width), outer_w - 2*(inset + width), h + 1, 3);
  }
}

module standoffs(l, w, z) {
  // 4-40 holes on SparkFun boards; M2.5 self-tappers fit happily.
  inset = 2.5;
  for (x = [inset, l - inset])
    for (y = [inset, w - inset])
      translate([x, y, 0])
        difference() {
          cylinder(d = 6, h = z);
          translate([0, 0, z - 6]) cylinder(d = 2.1, h = 7);
        }
}

// =====================================================================
//  LAYOUT - comment one out to export the other
// =====================================================================
base();
translate([0, outer_w + 12, 0]) lid();

// Sanity check: uncomment to see the interior volume the parts must fit in.
// echo(str("Interior ", inner_l, " x ", inner_w, " x ", inner_h, " mm"));
// echo(str("Exterior ", outer_l, " x ", outer_w, " x ", outer_h, " mm"));
