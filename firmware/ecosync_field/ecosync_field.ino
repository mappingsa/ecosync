/*
  ==================================================================================
  FieldPoint Environmental - FPE EcoSync Field System
  Production Software: Auto-Timeout Polygon Logic + Web Config + ESRI Sync
  Board: SparkFun Thing Plus - ESP32 WROOM (USB-C)
  Wiring: Spray Trigger (Pressure Pad) -> Pin 14 and GND

  ----------------------------------------------------------------------------------
  Revision 2026-08-20 -- production hardening pass.

    * All settings (including Wi-Fi credentials) now live in /config.txt on the SD
      card. Nothing sensitive is compiled into the binary. The old 4-line config
      format is migrated automatically on first boot.
    * If Wi-Fi cannot be joined, the unit raises its own access point so the
      config page is always reachable to fix it.
    * GPS runs in auto-PVT mode. Nothing blocks the main loop, and no point or
      polygon is ever recorded without a valid fix inside the accuracy limit.
    * Polygon vertices are distance-gated, so RAM use is bounded by area covered
      rather than by time spent spraying.
    * In-progress polygons survive disarming and mode changes.
    * Battery telemetry is polled on a timer, never inside a web request.
    * Unsynced counts are recovered from the SD card at boot.
    * ESRI sync batches features atomically and can never upload a duplicate.
  ==================================================================================
*/
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <math.h>
#include <sys/select.h>   // select() - used to test socket writability without blocking
#include <esp_system.h>   // esp_reset_reason() - why the last restart happened
#include <Preferences.h>  // boot counter, survives both reset and power cycle

// ==========================================
// 1. CONSTANTS
// ==========================================
const char* CONFIG_PATH   = "/config.txt";
const char* CONFIG_TMP    = "/config.tmp";   // half-written config lives here
const char* CONFIG_BAK    = "/config.bak";   // last known-good copy
const char* LASTPOINT_PATH = "/lastpoint.txt";
// Smallest plausible config. A file shorter than this is a truncated write, not
// a configuration, and must never be promoted over a good one.
const size_t CONFIG_MIN_BYTES = 200;
// How stale the saved last-point may be and still suppress a press. Long enough
// to cover a reboot mid-bush, short enough that yesterday's last plant cannot
// swallow this morning's first one.
const unsigned long LASTPOINT_MAX_AGE_S = 600;
const char* CA_CERT_PATH  = "/ca.pem";
const char* PENDING_PTS   = "/pending.csv";
const char* PENDING_POLY  = "/pending_poly.csv";
// Continuous track points go to their own file and their own layer. One
// morning of dodder puts hundreds of them in; mixed into the points layer they
// swamp the spot records and "points captured" on a spray report stops meaning
// anything. The points layer is WHAT was treated, the track layer is HOW it
// was covered, and the treated-area polygons are derived from the track.
const char* PENDING_TRACK = "/pending_track.csv";
const char* POLY_JOURNAL  = "/poly_journal.txt";
const char* TMP_PTS       = "/pending.tmp";
const char* TMP_POLY      = "/pending_poly.tmp";
const char* TMP_TRACK     = "/pending_track.tmp";
// LINE mode. A polyline is a ring that never closes: bait trails, and the
// "effort line" a Board logs to say which roads have actually been driven.
const char* PENDING_LINE  = "/pending_line.csv";
const char* TMP_LINE      = "/pending_line.tmp";

const int   SD_CS_PIN     = 5;
// Bump this on every build that leaves this machine. It prints at boot and
// shows on the dashboard, so the version actually running is never in doubt.
const char* FIRMWARE_BUILD = "20261114-k map colour by target";

const int   TRIGGER_PIN   = 14;

// GNSS over UART, for the DAN-F10N. The SparkFun breakout has no I2C at all -
// u-blox's own product summary lists "1 UART" and nothing else - so this is a
// required change, not a preference. Both boards are 3.3 V logic, no shifting.
//   breakout TXD -> GNSS_RX_PIN   (module talks, ESP32 listens)
//   breakout RXD -> GNSS_TX_PIN
//   breakout 3V3 -> Thing Plus 3V3, GND -> GND
// NOT the breakout's 5V pin: that feeds its own regulator, which wants 3.5 V
// minimum, and a LiPo spends the back half of its life below that.
const int   GNSS_RX_PIN   = 16;
const int   GNSS_TX_PIN   = 17;
const long  GNSS_BAUD     = 38400;   // DAN-F10N default, 8N1

// Which transport actually answered. The old M9 is I2C, the new F10N is UART,
// and while both boards exist on the bench one binary that copes with either
// is worth more than a tidier one that does not.
String      gnssTransport = "none";

// Are SBAS corrections actually being APPLIED right now? The DAN-F10N is quoted
// at 1.0 m CEP *with* SBAS and 1.5 m without, so the difference between those
// two figures is entirely this flag. An accuracy number alone cannot tell you
// which regime you are in - and "we use SouthPAN" is not a claim to make to DEW
// on the strength of a datasheet.
//
// diffSolnEverSeen is kept separately because SBAS comes and goes with sky view.
// "Never once since boot" and "had it, lost it under canopy" are different
// faults and want different responses.
// --- Power ---
// All three are config-backed so they can be tuned from the Advanced page
// without a reflash. Measured draw: 100 mA on the original code, 145 mA once
// the always-on AP and NMEA feed arrived, 243 mA with the F10N fitted - which
// is 8 hours on a 2000 mAh cell, and 7 hours is not a working day.
int         cpuMhz      = 80;    // 80/160/240. Wi-Fi needs 80 minimum.
int         uiRefreshS  = 10;    // dashboard auto-refresh; 2 s was costing real power
bool        wifiSleep   = true;  // Wi-Fi modem sleep between beacons when STA-only

bool        live_diff_soln    = false;
bool        diffSolnEverSeen  = false;
unsigned long diffSolnFirstMs = 0;   // how long after boot corrections first applied

// Service endpoints. These are not secrets, so they stay in the sketch.
// Destination layers. These were hardcoded until 28 Oct, which meant a unit
// could only ever post to one organisation's layers - a reflash per client, and
// no way to point a trial unit at a test service. They are now config.txt keys
// with these as the defaults, so an empty or missing key keeps today's
// behaviour exactly.
String esriPointServiceURL   = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Points/FeatureServer/0/addFeatures";
String esriPolygonServiceURL = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Polygons/FeatureServer/0/addFeatures";
String esriTrackServiceURL   = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Track/FeatureServer/0/addFeatures";
String esriLineServiceURL    = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/Weed_Spray_Lines/FeatureServer/0/addFeatures";
// WHICH UNIT THIS IS.
//
// Goes into job_id on every record, which is how two units' work is told
// apart in ArcGIS - and how track_to_polygons.py keeps their runs separate,
// since a run_id is only an epoch second and collides across units.
//
// It lives in config.txt as device_id so one binary serves every unit. It was
// a compile-time constant, which meant each unit needed its own build, and
// flashing the wrong one mislabelled every record it ever uploaded.
//
// DEFAULT_DEVICE_ID is the fallback when config.txt has no device_id line -
// a fresh or reformatted card cannot leave a unit with no identity at all.
const char* DEFAULT_DEVICE_ID     = "FPE_EcoSync_001";
String      currentJobID          = DEFAULT_DEVICE_ID;

// Timings
// polyTimeoutDelay and the point cooldown are configurable - see section 2.
const unsigned long polySampleInterval   = 1000;   // how often we consider a vertex
// Lockout after an ACCEPTED press.
//
// Was 100 ms, which is enough for contact chatter while the pad is being
// pressed but not for the bounce when it springs back. A bench run of 42
// deliberate press-release cycles on 24 October 2026 produced 50 interrupt
// fires - the extras all arriving on the RELEASE, 200-400 ms after the real
// press and so clear of a 100 ms window.
//
// No records were ever duplicated by this: the point cooldown suppressed every
// one as "same plant". But it inflated the press counter on the dashboard,
// which is the first number anyone reaches for to sanity-check a day, and it
// would become real duplicates under a short enough cooldown.
//
// 250 ms covers the release bounce and is still far quicker than anyone can
// deliberately press twice. A triple-press gesture would need about 750 ms,
// which is a natural fast tap-tap-tap.
const unsigned long triggerDebounceDelay = 250;

// Drain the GNSS UART four times as often. getPVT() is cheap when nothing new
// has arrived - it reads whatever bytes are waiting and returns - so the only
// thing a longer interval bought was a fuller receive buffer.
const unsigned long gpsPollInterval      = 50;     // auto-PVT check rate
const unsigned long gpsStaleAfter        = 5000;   // fix considered stale after this
const unsigned long batteryPollInterval  = 15000;
const unsigned long wifiRetryInterval    = 30000;
const unsigned long apFallbackMs         = 60000;  // station down this long -> start the AP

// Bounds
const int MAX_POLY_VERTICES = 1000;   // backstop; auto-splits when reached

// ONE pair of vertex buffers, shared by the polygon and the line builder.
//
// They were separate when LINE was added, which quietly took another 16 KB of
// static DRAM - 2 x 1000 doubles - away from a heap that TLS was already
// competing for. The symptom was not a crash: the first upload batch went
// through and every one after it failed with send-payload and connection-lost
// errors, because there was no longer enough free heap to stand up a second
// TLS session. Nothing calls both builders at once; syncPendingFile works one
// queue at a time, so sharing is safe.
static double shapeXs[MAX_POLY_VERTICES];
static double shapeYs[MAX_POLY_VERTICES];
const int POLY_COORD_RESERVE = 4096;  // pre-allocate to limit heap fragmentation

// ==========================================
// 2. CONFIGURABLE SETTINGS (all loaded from /config.txt)
// ==========================================
// --- Capture slots -----------------------------------------------------------
// Each dashboard button owns EVERYTHING about how that weed is captured: name,
// control method, capture mode, and the two numbers that mode needs. Binding
// the tuning to the target is the whole point - changing species used to mean
// editing the cooldown in config, in the field, with gloves on.
//
//   POINT       A = cooldown seconds       B = movement that releases it (m)
//   CONTINUOUS  A = seconds between fixes  B = minimum movement (m), 0 = off
//   POLYGON     A = vertex spacing (m)     B = idle timeout (s)
const int NUM_SLOTS = 4;
String slotWeed[NUM_SLOTS]    = { "Target A", "Target B", "Target C", "Target D" };
String slotControl[NUM_SLOTS] = { "Foliar Spray", "Foliar Spray", "Foliar Spray", "Foliar Spray" };
String slotMode[NUM_SLOTS]    = { "POINT", "POINT", "POINT", "CONTINUOUS" };
float  slotA[NUM_SLOTS]       = { -1.0, -1.0, -1.0, -1.0 };  // -1 = seed from the mode default
float  slotB[NUM_SLOTS]       = { -1.0, -1.0, -1.0, -1.0 };
int    currentSlot            = 0;
String currentJobNumber = "FPE_Job_001";

// Two networks, tried in turn.
//
// Slot 1 is the vehicle's Starlink; slot 2 is the operator's phone hotspot.
// Both matter, for different reasons. An officer who joins the UNIT's access
// point to see the dashboard loses internet on the phone, so Field Maps stops
// working - and checking a board's weed observation in the field is half the
// job. Inverting it fixes that: the unit joins the phone, the phone keeps
// cellular, and both the dashboard and Field Maps work at once.
//
// Alternating attempts rather than scanning: a blocking scan costs a second or
// more of the loop, and each join attempt already has its own timeout.
String wifiSsid   = "";          // blank -> device raises its own access point
String wifiPass   = "";
String wifiSsid2  = "";          // phone hotspot
String wifiPass2  = "";
int    wifiSlot   = 0;           // which of the two the next attempt uses
bool   apAlways   = true;         // keep the setup AP up even when joined to Wi-Fi,
                                 // so the config page is always reachable
String apSsid     = "FPE-EcoSync-Setup";
String apPass     = "fpeecosync"; // must be at least 8 chars or the AP is open

String esriToken  = "";          // optional ArcGIS API key / token; blank = anonymous
String adminUser  = "fpe";
String adminPass  = "";          // blank disables config-page auth

float  gpsMaxAccM       = 5.0;   // reject logging above this horizontal accuracy
float  polyMinVertexM   = 2.0;   // minimum movement before a new polygon vertex
float  polySimplifyM    = 2.0;   // Douglas-Peucker tolerance on upload; 0 disables
                                 // 2.0 measured best at ~1.2m GPS accuracy: heavy vertex
                                 // reduction, corners still within ~1.4m. 3.0+ cuts corners.

// --- Point de-duplication ---------------------------------------------------
// A single large boxthorn can take twenty trigger pulls. Without a guard that
// becomes twenty stacked points on one plant. A press is logged when EITHER
// enough time has passed OR the operator has moved far enough to be at a
// different plant - so working one bush stays a single record, while moving
// straight to the next bush still logs immediately.
float pointCooldownS = 10.0;  // seconds on the same spot before a second point
float pointMinDistM  = 5.0;   // metres of movement that releases the lock early
                              // 0 disables the distance rule (pure timer)

// --- Polygon idle timeout ----------------------------------------------------
float polyTimeoutS   = 15.0;  // seconds of trigger silence that closes a polygon

// --- NMEA feed to phone apps -------------------------------------------------
// Serves the receiver's own NMEA 0183 stream over TCP, so ArcGIS QuickCapture
// or Survey123 on a phone can use this unit's position instead of the handset's.
// Esri's "Network connection" receiver option needs no Apple MFi certification,
// unlike the Bluetooth path.
bool nmeaOut  = true;
int  nmeaPort = 10110;      // conventional port for NMEA 0183 over TCP

// --- Wi-Fi transmit power ----------------------------------------------------
// The original firmware pinned this at 8.5 dBm, which is roughly a tenth of the
// module's full output. That is fine beside the router and marginal across a
// paddock. Configurable so it can be raised when the signal is weak, but left
// at the original value by default so nothing changes unexpectedly.
float wifiTxDbm      = 8.5;   // 2.0 to 19.5 dBm

// Control method is recorded against each target and written to control_type in
// ArcGIS. It now lives in slotControl[] beside the weed it belongs to - see the
// capture slots above.

// ==========================================
// 3. RUNTIME STATE
// ==========================================
int    unsyncedPoints   = 0;
int    unsyncedPolygons = 0;
int    unsyncedTrack    = 0;
int    unsyncedLines    = 0;
String lastSyncStatus   = "Ready for first sync";
String lastAction       = "System Booted";
bool   systemArmed      = false;
bool   triggerSync      = false;
bool   sdReady          = false;

// --- Boot and reset diagnostics -------------------------------------------
// systemArmed starts false, so EVERY restart looks exactly like "the safety
// came on by itself". With no uptime, no boot count and no reset reason on the
// dashboard, a reboot was invisible and the symptom got blamed on the safety
// logic instead. esp_reset_reason() tells us precisely which it was - and in
// particular distinguishes ESP_RST_EXT (something pulled the EN line low, e.g.
// the Fenix wire) from a brownout or a firmware crash.
String        bootResetReason   = "unknown";
int           bootResetCode     = 0;
bool          bootWasUnexpected = false;   // anything other than a clean power-on
// Separate from the flag above on purpose. Dismissing the banner must not
// erase WHY the unit restarted - that figure is still wanted on the
// diagnostics page and in the boot log long after the notice has been read.
bool          bootNoticeDismissed = false;
unsigned long bootCount         = 0;
Preferences   bootPrefs;

// Counts browser prefetch / prerender requests that were refused. If this
// climbs while the dashboard is open, a speculative browser fetch was the
// cause rather than a reset, and the two are then trivially told apart.
unsigned long speculativeBlocked = 0;

String currentWeed        = "";
String currentControlType = "";   // follows the selected target
String loggingMode        = "POINT";

// --- Cached GPS state (written only by pollGPS) ---
bool          live_gps_lock    = false;
bool          live_time_valid  = false;
double        live_lat         = 0.0;
double        live_lon         = 0.0;
float         live_accuracy_m  = 0.0;
uint8_t       live_siv         = 0;
uint8_t       live_fix_type    = 0;
unsigned long live_epoch       = 0;
unsigned long lastGpsUpdate    = 0;

// --- Cached battery state (written only by pollBattery) ---
bool  batteryOk        = false;
int   live_battery_pct = 0;
float live_charge_rate = 0.0;

// --- Polygon state ---
bool          polygonActive   = false;
// LINE reuses the whole polygon machinery - vertex gating, the crash journal,
// the simplifier - because a line IS a ring minus the closing step. This flag
// is the only thing that differs while one is being recorded, and it is pinned
// at the start so changing mode mid-capture cannot turn a line into a ring.
bool          shapeIsLine     = false;
String        currentPolygonCoords   = "";
String        currentPolygonWeed     = "";
String        currentPolygonControl  = "";
int           vertexCount     = 0;
unsigned long polyStartTime   = 0;
unsigned long lastPolySampleTime = 0;
unsigned long lastTriggerActivity = 0;
double        lastVertexLat   = 0.0;
double        lastVertexLon   = 0.0;

// Where the trigger was first pulled. Kept separately from lastVertex* so a
// polygon too short to be a ring can still be saved as a point whose position
// and timestamp agree with each other.
double        polyStartLat    = 0.0;
double        polyStartLon    = 0.0;

// What actually happened to the polygons this session. The unit is used
// hands-free, so "did that work?" cannot be answered by watching a screen
// during the run - it has to be countable afterwards.
unsigned long polySaved     = 0;   // written as a ring
unsigned long polyAsPoint   = 0;   // too short to be a ring, kept as a point
unsigned long polyDiscarded = 0;   // nothing recorded

// --- Continuous capture state ---
// CONTINUOUS watches the trigger's STATE, not its edge. An edge is a moment,
// and a moment can fall inside the ~120 ms this loop spends building the
// dashboard page for a phone - which is how presses went missing. A trigger
// held for two seconds cannot be missed by anything sampling at 1 ms.
unsigned long currentRunId      = 0;   // epoch second the trigger went down; 0 = idle
unsigned long lastContinuousMs  = 0;
double        lastContinuousLat = 0.0;
double        lastContinuousLon = 0.0;
unsigned long continuousPoints  = 0;   // points in the CURRENT run
unsigned long continuousRuns    = 0;   // runs completed this session
unsigned long continuousTotal   = 0;   // points written this session
unsigned long runReleasedAt     = 0;   // millis() the trigger came up; 0 = held

// A run also ends if you have walked this far from its last point while the
// trigger is up - otherwise a short release hold would merge two infestations
// you walked between. Fixed rather than configurable: anything tighter than
// this could be tripped by receiver noise alone at 2-3 m accuracy.
const float RUN_END_MOVE_M = 15.0;

// --- Trigger latch ---
// Raised by the pin interrupt so no press can be lost to a slow loop pass. The
// ISR does nothing else: debouncing happens in loop(), which keeps millis() out
// of interrupt context entirely.
// The INTERRUPT timestamps the press; the loop only reads the clock it kept.
//
// Two earlier attempts failed here and both are worth remembering.
//
// Polling the pin (26 Oct) lost presses in the field: serving the dashboard
// stretches a loop pass to 300-800 ms, and a quick press begins and ends
// inside one, so there was nothing left to measure.
//
// Pairing edges in the ISR (7 Nov, morning) failed worse - nothing logged at
// all. It assumed a falling edge opens a closure and a rising edge closes it.
// With this pad there are about ten edges per press, so a bounce closed the
// measurement a millisecond after it opened; the pin then sat LOW for the real
// 300 ms of the press without generating another edge, and that press was
// never timed. Every closure came out at 1-6 ms.
//
// So: no pairing. The ISR records only WHEN the first LOW of a closure
// happened and WHEN the most recent edge of any kind happened. The loop
// decides a press is over once the pin is sitting HIGH and has been quiet for
// the settle time, and the duration is the difference between those two
// timestamps. Chatter cannot desynchronise it because nothing is paired, and a
// late loop only logs the point late - it cannot change what was measured.
volatile bool          triggerLatched = false;   // kept: "an edge happened"
volatile unsigned long isrFirstLow    = 0;       // first LOW of the closure in progress
volatile unsigned long isrLastEdge    = 0;       // most recent edge, of either direction
portMUX_TYPE           triggerMux     = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR triggerISR() {
  unsigned long now = millis();
  portENTER_CRITICAL_ISR(&triggerMux);
  triggerLatched = true;
  isrLastEdge    = now;
  if (digitalRead(TRIGGER_PIN) == LOW && isrFirstLow == 0) isrFirstLow = now;
  portEXIT_CRITICAL_ISR(&triggerMux);
}

// Pulls that did nothing because the safety was on. Previously silent - no
// counter, no message - so a disarmed unit was indistinguishable from a broken
// one until you looked at the button.
unsigned long pullsWhileDisarmed = 0;

// millis() when the station link was last seen up. Drives the access-point
// fallback in maintainWiFi().
unsigned long wifiDownSince = 0;

// Retry state. wifiRetryPending marks the gap between dropping a stuck join
// attempt and starting a fresh one - see maintainWiFi().
bool          wifiRetryPending = false;
unsigned long wifiJoinAttempts = 0;

// --- Access point power management ---
//
// An AP must stay awake to beacon, so it also blocks modem sleep on the radio.
// Roughly 45 mA, which on a 2000 mAh cell is about 2.25 %/h - it doubled the
// unit's drain when the fallback started leaving one up all day out of range.
//
// So the AP stops after apIdleStopMin with nobody attached, and then reopens
// for apWindowSec every apWindowMin so there is always a way back in.
//
// The window exists because the trigger gesture cannot be relied on. It is
// gated on the safety being ON, and the safety is a DASHBOARD control - there
// is no physical switch. Arm the unit, spray, let the AP idle out, and the only
// way to disarm is the page you have just lost. The gesture is unreachable in
// exactly the situation it was written for. A second case it never covered at
// all: unit happily on Starlink, phone on mobile data, so nothing is "down" and
// nothing brings the AP back.
//
// Hence a window that opens on a clock and needs nobody's cooperation. Two
// minutes in every fifteen is 13% of 45 mA, about 0.3 %/h, against 2.25 %/h to
// leave it up. Connect during a window and it stays up - the idle clock takes
// over from there.
unsigned long apLastClientMs = 0;    // millis() a client was last attached
bool          apSuppressed   = false; // idle-stopped; no fallback restart
unsigned long triggerHeldSince = 0;   // for the summon gesture
unsigned long apStoppedAt    = 0;    // millis() the AP last went down
unsigned long apWindowStartedAt = 0; // non-zero while a peek window is open
float         apIdleStopMin  = 10.0;  // 0 disables the idle shutdown
float         apWindowMin    = 15.0;  // 0 disables the periodic window
float         apWindowSec    = 120.0; // how long each window stays open
const unsigned long AP_SUMMON_HOLD_MS = 5000;

// --- Point de-dup state ---
//
// These used to live only in RAM, so every reset handed out one free record:
// hasLoggedPoint came back false and the first trigger edge logged with no
// cooldown and no distance check. On a unit with an intermittent power
// connection that is one spurious point per boot, which is how 14 October 2026
// produced 87 records in bursts 3-4 seconds apart while standing still.
// Now they survive a reboot via /lastpoint.txt.
double lastPointLat   = 0.0;
double lastPointLon   = 0.0;
bool   hasLoggedPoint = false;
unsigned long lastPointEpoch = 0;     // GNSS time of the last logged point

// --- Trigger safety at boot ---
//
// The ISR is attached FALLING. Pin state during reset and the moment pinMode
// engages the pull-up can both present as an edge, and a pad resting closed at
// power-on looks like a press that never ended. Nothing is accepted until the
// trigger has been SEEN released at least once.
// Set once the trigger has been seen RELEASED and settled, cleared the moment
// a press is accepted. One accepted press per physical press-release cycle,
// regardless of how long the press is held.
//
// A plain time lockout cannot do this. A bench run of 50 deliberate presses on
// the Olight pad produced 539 falling edges and 53 accepted at a 250 ms
// lockout: conductive rubber chatters as the thumb rolls off, and that chatter
// lands AFTER the lockout has expired if the press was held longer than the
// lockout. Raising the timeout just moves the hold duration at which it fails.
bool triggerSeenReleased = false;
unsigned long triggerHighSince = 0;

// --- HOLD TO LOG ---------------------------------------------------------
//
// The Olight pad sits proud enough that a finger resting on the spray trigger
// brushes it, and on 26 Oct that was logging points nobody meant. The pad is a
// plain on/off contact, so there is no pressure to read: a brush and a squeeze
// are electrically identical. What differs is HOW LONG the contact stays shut,
// so that is what gets measured.
//
// The pad bounces open mid-press - the 20-press log on 25 Oct showed the pin
// genuinely re-opening inside a single squeeze - so the measurement cannot be
// "shut continuously for N ms". The ISR instead treats a closure as running
// until the pin settles high, and reports the LONGEST closure it saw since the
// loop last looked, which steps over both mid-press dropouts and the burst of
// short closures that follows a release.
//
// This was polled until 7 Nov. See the note on the ISR for why it is not.
unsigned long triggerTooBrief   = 0;      // closures rejected as a brush

// How long the pin must sit HIGH before a release counts as settled. Long
// enough to outlast contact chatter, far shorter than anyone can press twice.
const unsigned long triggerReleaseSettleMs = 60;

// Set when the config could not be read and defaults are in use, so the
// dashboard can say so instead of quietly looking normal.
String configWarning = "";

// --- Trigger state ---
int           lastTriggerRaw      = HIGH;
int           stableTriggerState  = HIGH;
unsigned long lastTriggerDebounce = 0;
unsigned long lastLogTime         = 0;

// --- Networking state ---
bool          apMode          = false;
unsigned long lastWifiAttempt = 0;
String        caCert          = "";   // PEM loaded from SD; blank = insecure fallback

// --- NMEA relay state ---
WiFiServer nmeaServer(10110);
WiFiClient nmeaClient;
bool       nmeaServerStarted = false;
bool       nmeaSeenGGA = false;   // position + fix quality
bool       nmeaSeenRMC = false;   // position + time
bool       nmeaSeenGST = false;   // accuracy estimate - what drives the app's
                                  // quality indicator, and what an audit relies on
String     nmeaLastSentence = "";
unsigned long nmeaBytesSent = 0;

// Every distinct 3-letter sentence type the module has actually emitted, e.g.
// "GGA GSA GSV RMC VTG". Three yes/no flags cannot distinguish "the module does
// not support GST" from "NMEA is not reaching us at all"; this list can.
String     nmeaTypesSeen = "";

// Did the module ACCEPT the request to turn GST on? The return value used to be
// discarded, so a rejected request looked identical to a module that simply
// never sends GST. Now we record it and show it on the diagnostics page.
// Did the module accept the SouthPAN configuration? Recorded so a partial
// failure is visible rather than silently halving the accuracy.
bool       sbasConfigOk       = false;
String     sbasConfigDetail   = "not attempted";

bool       gstEnableOk        = false;
bool       gstEnableAttempted = false;
String     gnssProtVer        = "";   // PROTVER identifies the module generation

// Sentences discarded because the phone's socket was not ready to accept them.
// Dropping is CORRECT: a position fix is worth nothing a second later, whereas
// waiting for the socket stalls spray logging. See the note on NmeaRelay.
unsigned long nmeaDropped = 0;
unsigned long nmeaStalls  = 0;   // writes that took too long; client is dropped

// Trigger pulls refused because the fix was unusable. A hands-free device that
// silently declines to log is the worst failure it can have: on 26 Aug a whole
// morning was lost to gps_max_acc=2.0 while every real reading was 2.2-3.3 m.
// Counting them makes the loss visible without watching the screen.
unsigned long rejectedPulls   = 0;
String        lastRejectReason = "";

// Raw pin activity, counted and printed BEFORE any arming, debounce or GPS
// gate. If these never move, the problem is the pad or the wiring and no amount
// of firmware reading will find it. If they move but nothing logs, it is us.
unsigned long triggerRawChanges = 0;
// Raw interrupt fires, bounce included. A diagnostic, not an operator figure:
// a mechanical pad bounces on release, so this reads roughly 30% above the
// number of real presses. Kept as the cross-check against triggerRawChanges.
unsigned long triggerPresses    = 0;
// Presses the debounce gate ACCEPTED, and the ones it threw away. These exist
// because the edge message could not answer "did that press count?". It printed
// a pass-local flag, and only on passes where the POLLED level had changed - so
// with a chattering pad the accepted edge was usually consumed on some other
// pass and the line read fired=no for a press that had in fact been taken.
unsigned long triggerAccepted   = 0;
unsigned long triggerRefused    = 0;

// How long the pad must be held shut before a press counts. Lives in
// config.txt so it can be tuned in the paddock by feel instead of by reflash.
unsigned long triggerHoldMs     = 150;

// Points actually written this session. This is what an operator wants - "how
// many have I got" - and until now nothing on the dashboard answered it for a
// POINT-mode day, because the session summary only appeared when there were
// polygons or continuous runs to report.
unsigned long pointsLogged      = 0;

// Where the unit has actually captured something this session.
//
// The map could originally only show two things: a line WHILE it was being
// recorded, and points that had already reached ArcGIS. So a finished line, or
// a morning's points before a sync, showed nothing at all - which is the first
// thing anyone checks the map for.
//
// 200 positions at 16 bytes is 3.2 KB. Deliberately modest: the last time
// something took static RAM without asking, TLS stopped being able to open a
// connection and the sync died.
const int TRAIL_MAX = 200;
double        trailLat[TRAIL_MAX];
double        trailLon[TRAIL_MAX];
int           trailCount = 0;    // how many are valid
int           trailNext  = 0;    // ring cursor
// 0 means a standalone point; anything higher is a shape, and vertices sharing
// a number belong to the same line or ring. Without this every line vertex was
// drawn as its own dot, so a captured line looked like a scatter of marks and
// was indistinguishable from the points beside it.
int           trailSeg[TRAIL_MAX];
int           trailSlot[TRAIL_MAX];   // which target was selected at the time
int           trailSegNow  = 0;  // shape being captured right now
int           trailSegLast = 0;  // highest shape number handed out
void trailAdd(double lat, double lon, int seg) {
  if (lat == 0.0 && lon == 0.0) return;
  trailLat[trailNext]  = lat;
  trailLon[trailNext]  = lon;
  trailSeg[trailNext]  = seg;
  trailSlot[trailNext] = currentSlot;
  trailNext = (trailNext + 1) % TRAIL_MAX;
  if (trailCount < TRAIL_MAX) trailCount++;
}
unsigned long lastAcceptedPress = 0;

// How long one pass of loop() takes. The old trigger scheme assumed the loop
// was fast relative to a press; if it was not, presses were silently dropped.
unsigned long loopLastMs = 0;
unsigned long loopMaxMs  = 0;

// Worst horizontal accuracy seen while the current polygon was open. The WORST,
// not the mean: "no vertex on this boundary was worse than X" is defensible in
// an audit, where a mean quietly hides the bad vertex that actually matters.
double polyWorstAccM = 0.0;

WebServer server(80);
SFE_UBLOX_GNSS myGNSS;
SFE_MAX1704X lipo(MAX1704X_MAX17048);

// ==========================================
// 4. SMALL HELPERS
// ==========================================

// Flat-earth approximation. Accurate to well under 1% at the distances
// involved here (metres), and far cheaper than haversine.
double metresBetween(double lat1, double lon1, double lat2, double lon2) {
  const double M_PER_DEG = 111320.0;
  double dLat = (lat2 - lat1) * M_PER_DEG;
  double dLon = (lon2 - lon1) * M_PER_DEG * cos(lat1 * 0.017453292519943295);
  return sqrt(dLat * dLat + dLon * dLon);
}

// Can this socket accept data RIGHT NOW, without waiting?
//
// This exists because NetworkClient::write() is not safe to call from the main
// loop. Internally it runs select() with a ONE SECOND timeout and retries that
// up to TEN times, so one write on a socket that is not ready can block for ten
// seconds. The first version of this relay called write() once PER BYTE, which
// put ~600 of those calls per second inside the GNSS library's byte pump - which
// is called from pollGPS() in loop(). The result in the field was exactly what
// you would predict: the dashboard froze and the spray trigger stopped logging
// whenever a phone was attached, and only a power cycle cleared it.
//
// So we test writability ourselves with a ZERO timeout and skip the sentence if
// the socket is not ready. NetworkClient::fd() is public, which makes this a few
// lines rather than a redesign.
static bool nmeaSocketWritable(int fd) {
#ifdef ECOSYNC_HOST_TEST
  (void)fd;
  return NMEA_TEST_SOCKET_WRITABLE;   // host harness drives both paths
#else
  if (fd < 0) return false;
  fd_set set;
  struct timeval tv;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  tv.tv_sec  = 0;
  tv.tv_usec = 0;
  return select(fd + 1, NULL, &set, NULL, &tv) > 0 && FD_ISSET(fd, &set);
#endif
}

// Receives NMEA characters from the GNSS library and forwards them to whichever
// phone is connected, if any. Also watches which sentence types are flowing, so
// the diagnostics page can prove GST (accuracy) is present rather than assuming.
//
// Rule for this class: it must NEVER block. It runs inside the GNSS library's
// parser, which runs inside loop(). Anything slow here stops spray logging.
class NmeaRelay : public Stream {
public:
  int  available() override { return 0; }
  int  read()      override { return -1; }
  int  peek()      override { return -1; }
  void flush()     override {}

  size_t write(uint8_t c) override {
    // Track sentence types regardless of whether anyone is listening.
    if (c == '\n' || c == '\r') {
      if (buf.length() > 6) {
        if (buf.indexOf("GGA") > 0) nmeaSeenGGA = true;
        if (buf.indexOf("RMC") > 0) nmeaSeenRMC = true;
        if (buf.indexOf("GST") > 0) nmeaSeenGST = true;

        // Sentences arrive as $ttXXX,... - talker in 1..2, type in 3..5.
        // Collect the distinct types so the diagnostics page can show exactly
        // what the module emits rather than only what we hoped for.
        if (buf.charAt(0) == '$' && nmeaTypesSeen.length() < 80) {
          String type = buf.substring(3, 6);
          if (nmeaTypesSeen.indexOf(type) < 0) {
            if (nmeaTypesSeen.length()) nmeaTypesSeen += " ";
            nmeaTypesSeen += type;
          }
        }
        nmeaLastSentence = buf;
        sendSentence(buf);      // ONE socket write per sentence, never per byte
      }
      buf = "";
    } else if (buf.length() < 100) {
      buf += (char)c;
    }

    return 1;   // always report success, so the library can never stall on us
  }

  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; i++) write(b[i]);
    return n;
  }

private:
  // Push one complete sentence, or drop it. Never wait.
  void sendSentence(const String &sentence) {
    if (!(nmeaClient && nmeaClient.connected())) return;

    if (!nmeaSocketWritable(nmeaClient.fd())) {
      nmeaDropped++;      // phone is not keeping up - skip this fix, not the next
      return;
    }

    String out = sentence + "\r\n";
    unsigned long t0 = millis();
    nmeaClient.write((const uint8_t *)out.c_str(), out.length());
    unsigned long took = millis() - t0;

    if (took > 250) {
      // select() said ready but the write still dragged. A phone that cannot
      // take eighty bytes promptly is not usefully connected, and logging
      // matters more than the feed. Drop it; it can reconnect.
      nmeaStalls++;
      nmeaClient.stop();
      Serial.println("NMEA feed: client stalled the loop, disconnected.");
      return;
    }
    nmeaBytesSent += out.length();
  }

private:
  String buf;
};
NmeaRelay nmeaRelay;

// The ESP32 power enum is simply dBm x 4, so this maps cleanly.
// The access point name this unit uses when config.txt does not set one.
// Derived from the device id so two units are never both "FPE-EcoSync-Setup" -
// same name, same password, and a phone silently joins the stronger of the
// two. You then configure the wrong unit with no error and no clue.
String defaultApSsid() {
  String s = currentJobID;
  s.replace("_", "-");
  return s + "-Setup";
}

// mDNS hostname, also derived - both units answering to ecosyncfield.local on
// the depot network is a fight neither of them wins. "FPE_EcoSync_001" gives
// ecosync001.local. Anything without a trailing number keeps the old name.
String mdnsHostname() {
  int end = currentJobID.length();
  int start = end;
  while (start > 0 && isdigit(currentJobID.charAt(start - 1))) start--;
  if (start == end) return "ecosyncfield";
  return "ecosync" + currentJobID.substring(start);
}

wifi_power_t txPowerSetting() {
  float d = wifiTxDbm;
  if (d < 2.0)  d = 2.0;
  if (d > 19.5) d = 19.5;
  return (wifi_power_t)(int)(d * 4.0);
}

// A target name going into a JavaScript string that itself sits inside a
// single-quoted HTML attribute. Three layers, so three things to get right:
// a backslash or a double quote would break the JS string, and an apostrophe -
// "Cat's Claw" is a real weed - would end the attribute early and silently
// mangle the button.
String jsEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '\\': out += "\\\\";  break;
      case '"':  out += "\\\"";  break;
      case '\'': out += "&#39;";  break;   // entity: the attribute is single-quoted
      case '<':  out += "&lt;";    break;
      case '&':  out += "&amp;";   break;
      default:   out += c;
    }
  }
  return out;
}

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:  out += c;
    }
  }
  return out;
}

// True only when we have a fix we are willing to record against.
bool gpsUsable() {
  if (!live_gps_lock) return false;
  if (!live_time_valid) return false;
  if (millis() - lastGpsUpdate > gpsStaleAfter) return false;
  if (live_accuracy_m <= 0.0 || live_accuracy_m > gpsMaxAccM) return false;
  if (live_lat == 0.0 && live_lon == 0.0) return false;
  return true;
}

// Why the current fix is unusable, for the operator.
String gpsRejectReason() {
  if (!live_gps_lock)                              return "No GPS fix";
  if (millis() - lastGpsUpdate > gpsStaleAfter)    return "GPS data stale";
  if (!live_time_valid)                            return "No GPS time";
  if (live_accuracy_m <= 0.0)                      return "No accuracy estimate";
  if (live_accuracy_m > gpsMaxAccM)                return "Accuracy " + String(live_accuracy_m, 1) + "m > " + String(gpsMaxAccM, 1) + "m limit";
  return "Position invalid";
}

// ==========================================
// 5. SD CONFIGURATION  (key=value, with migration from the old 4-line format)
// ==========================================
static void applyConfigLine(const String &line) {
  if (line.length() == 0 || line.charAt(0) == '#') return;
  int eq = line.indexOf('=');
  if (eq <= 0) return;

  String k = line.substring(0, eq);      k.trim();
  String v = line.substring(eq + 1);     v.trim();

  // Slot keys are target1..4, control1..4, mode1..4, tunea1..4, tuneb1..4.
  // The target/control names are unchanged from the old format, so an existing
  // config.txt keeps working and simply gains a fourth slot on first save.
  if (k.startsWith("target") && k.length() == 7) {
    int i = k.charAt(6) - '1';
    if (i >= 0 && i < NUM_SLOTS) slotWeed[i] = v;
  }
  else if (k.startsWith("control") && k.length() == 8) {
    int i = k.charAt(7) - '1';
    if (i >= 0 && i < NUM_SLOTS) slotControl[i] = v;
  }
  else if (k.startsWith("mode") && k.length() == 5) {
    int i = k.charAt(4) - '1';
    String m = v; m.toUpperCase();
    if (i >= 0 && i < NUM_SLOTS &&
        (m == "POINT" || m == "CONTINUOUS" || m == "POLYGON" ||
         m == "LINE")) slotMode[i] = m;
  }
  else if (k.startsWith("tunea") && k.length() == 6) {
    int i = k.charAt(5) - '1';
    if (i >= 0 && i < NUM_SLOTS) slotA[i] = v.toFloat();
  }
  else if (k.startsWith("tuneb") && k.length() == 6) {
    int i = k.charAt(5) - '1';
    if (i >= 0 && i < NUM_SLOTS) slotB[i] = v.toFloat();
  }
  else if (k == "device_id")    currentJobID     = v;
  else if (k == "job")          currentJobNumber = v;
  else if (k == "wifi_ssid")    wifiSsid         = v;
  else if (k == "wifi_pass")    wifiPass         = v;
  else if (k == "wifi_ssid2")   wifiSsid2        = v;
  else if (k == "wifi_pass2")   wifiPass2        = v;
  else if (k == "ap_always")    apAlways         = (v == "1" || v == "true" || v == "yes");
  else if (k == "ap_idle_stop") apIdleStopMin    = v.toFloat();
  else if (k == "ap_window_min") apWindowMin     = v.toFloat();
  else if (k == "ap_window_sec") apWindowSec     = v.toFloat();
  else if (k == "ap_ssid")      apSsid           = v;
  else if (k == "ap_pass")      apPass           = v;
  else if (k == "esri_token")   esriToken        = v;
  else if (k == "admin_user")   adminUser        = v;
  else if (k == "admin_pass")   adminPass        = v;
  else if (k == "gps_max_acc")  gpsMaxAccM       = v.toFloat();
  else if (k == "poly_min_dist") polyMinVertexM  = v.toFloat();
  else if (k == "poly_simplify") polySimplifyM   = v.toFloat();
  else if (k == "poly_timeout")  polyTimeoutS    = v.toFloat();
  else if (k == "point_cooldown") pointCooldownS = v.toFloat();
  else if (k == "point_min_dist") pointMinDistM  = v.toFloat();
  else if (k == "trigger_hold_ms") triggerHoldMs = (unsigned long)v.toInt();
  else if (k == "esri_point_url") { if (v.length()) esriPointServiceURL   = v; }
  else if (k == "esri_poly_url")  { if (v.length()) esriPolygonServiceURL = v; }
  else if (k == "esri_track_url") { if (v.length()) esriTrackServiceURL   = v; }
  else if (k == "esri_line_url")  { if (v.length()) esriLineServiceURL    = v; }
  else if (k == "wifi_tx_dbm")   wifiTxDbm       = v.toFloat();
  else if (k == "cpu_mhz")       cpuMhz           = v.toInt();
  else if (k == "ui_refresh")    uiRefreshS       = v.toInt();
  else if (k == "wifi_sleep")    wifiSleep        = (v == "1" || v == "true" || v == "yes");
  else if (k == "nmea_out")      nmeaOut          = (v == "1" || v == "true" || v == "yes");
  else if (k == "nmea_port")     nmeaPort         = v.toInt();
}

static void sanitiseConfig() {
  const char* defaultWeed[NUM_SLOTS] = { "Target A", "Target B", "Target C", "Target D" };
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (slotWeed[i].length() == 0)    slotWeed[i]    = defaultWeed[i];
    if (slotControl[i].length() == 0) slotControl[i] = "Foliar Spray";
    if (slotMode[i] != "POINT" && slotMode[i] != "CONTINUOUS" &&
        slotMode[i] != "POLYGON" && slotMode[i] != "LINE") {
      slotMode[i] = "POINT";
    }
    // A slot with no explicit tuning inherits the old global values, so a card
    // written by earlier firmware behaves exactly as it did before.
    if (slotA[i] < 0.0) {
      if      (slotMode[i] == "POINT")      slotA[i] = pointCooldownS;
      else if (slotMode[i] == "CONTINUOUS") slotA[i] = 1.0;
      else if (slotMode[i] == "LINE")       slotA[i] = 5.0;   // vertex spacing
      else                                  slotA[i] = polyMinVertexM;
    }
    if (slotB[i] < 0.0) {
      if      (slotMode[i] == "POINT")      slotB[i] = pointMinDistM;
      else if (slotMode[i] == "CONTINUOUS") slotB[i] = 8.0;
      // 0 = no idle timeout. A bait run stops to refill, and a timeout would
      // chop one trail into several. The second press is what ends a line.
      else if (slotMode[i] == "LINE")       slotB[i] = 0.0;
      else                                  slotB[i] = polyTimeoutS;
    }
    // Bounds. A continuous interval under half a second would flood the card.
    //
    // The lower bound on the hold also migrates a card written by the previous
    // build, where this number meant "minimum movement between fixes" and was
    // normally 0. Zero is not a sensible release hold, so it becomes 8 s.
    if (slotMode[i] == "CONTINUOUS") {
      if (slotA[i] < 0.5 || slotA[i] > 60.0)  slotA[i] = 1.0;
      if (slotB[i] < 1.0 || slotB[i] > 120.0) slotB[i] = 8.0;
    } else {
      if (slotA[i] < 0.0 || slotA[i] > 600.0) slotA[i] = 5.0;
      if (slotB[i] < 0.0 || slotB[i] > 600.0) slotB[i] = 5.0;
    }
  }
  if (currentSlot < 0 || currentSlot >= NUM_SLOTS) currentSlot = 0;

  // A unit with no identity would upload records that cannot be attributed to
  // anything, so a blank device_id falls back to the compiled default rather
  // than being accepted. Spaces and quotes come out because this value is
  // written into JSON and into an SSID.
  currentJobID.trim();
  currentJobID.replace("\"", "");
  currentJobID.replace(" ", "_");
  if (currentJobID.length() == 0) currentJobID = DEFAULT_DEVICE_ID;

  if (currentJobNumber.length() == 0) currentJobNumber = "FPE_Job_001";
  if (apSsid.length() == 0)           apSsid = defaultApSsid();
  if (apPass.length() < 8)            apPass = "fpeecosync";   // WPA2 minimum
  if (apIdleStopMin < 0.0 || apIdleStopMin > 120.0) apIdleStopMin = 10.0;
  if (apWindowMin   < 0.0 || apWindowMin   > 240.0) apWindowMin   = 15.0;
  // Below 30 s there is no time to find the SSID and open the page; above ten
  // minutes it is not a window, it is just an AP with gaps in it.
  if (apWindowSec  < 30.0 || apWindowSec   > 600.0) apWindowSec   = 120.0;
  if (adminUser.length() == 0)        adminUser = "fpe";
  // 15 m is a SANITY limit, not a quality bar. Quality is now recorded per
  // record in gps_accuracy_m and filtered in ArcGIS, where the decision is
  // reversible. A tight gate here throws data away in the scrub, permanently,
  // with nothing to show it happened - which is exactly what 2.0 m did.
  if (gpsMaxAccM <= 0.0 || gpsMaxAccM > 100.0) gpsMaxAccM = 15.0;
  if (polyMinVertexM < 0.0 || polyMinVertexM > 100.0) polyMinVertexM = 2.0;
  if (polySimplifyM < 0.0 || polySimplifyM > 50.0)    polySimplifyM = 2.0;
  // 3s floor on the polygon timeout: below that, normal pauses between pulls
  // would chop one patch into many polygons.
  if (polyTimeoutS < 3.0 || polyTimeoutS > 300.0)     polyTimeoutS = 15.0;
  if (pointCooldownS < 0.0 || pointCooldownS > 600.0) pointCooldownS = 10.0;
  if (pointMinDistM < 0.0 || pointMinDistM > 1000.0)  pointMinDistM = 5.0;
  // 0 would log on the faintest touch, which is the fault this exists to fix.
  // Above 2 s a normal squeeze would be thrown away as a brush.
  // 0 is legal and means "do not measure at all" - accept on the edge, the way
  // the build that ran a 1,174-point day did. It is the fallback if duration
  // measurement ever misbehaves in the field again.
  if (triggerHoldMs != 0 && (triggerHoldMs < 20 || triggerHoldMs > 2000)) triggerHoldMs = 150;
  if (wifiTxDbm < 2.0 || wifiTxDbm > 19.5)            wifiTxDbm = 8.5;
  if (nmeaPort < 1024 || nmeaPort > 65535)            nmeaPort = 10110;
  // Only these three are valid ESP32 crystal-derived speeds, and Wi-Fi will not
  // run below 80 MHz at all - a lower value would silently kill networking.
  if (cpuMhz != 80 && cpuMhz != 160 && cpuMhz != 240)  cpuMhz = 80;
  // Below 2 s the page reloads faster than it renders; above 60 s it feels dead.
  if (uiRefreshS < 2 || uiRefreshS > 60)               uiRefreshS = 10;
}

// Which network the next join attempt should use, and its label for the log
// and the dashboard. Slots with no SSID are skipped, so a unit with only one
// configured behaves exactly as it did before.
bool wifiSlotConfigured(int slot) {
  return slot == 0 ? wifiSsid.length() > 0 : wifiSsid2.length() > 0;
}

String wifiSlotSsid(int slot)  { return slot == 0 ? wifiSsid : wifiSsid2; }
String wifiSlotPass(int slot)  { return slot == 0 ? wifiPass : wifiPass2; }
String wifiSlotLabel(int slot) { return slot == 0 ? "Starlink" : "phone hotspot"; }

// Move to the other slot if it is configured. Called after a failed attempt so
// the unit alternates instead of hammering one network that is not there.
void wifiNextSlot() {
  int other = wifiSlot == 0 ? 1 : 0;
  if (wifiSlotConfigured(other)) wifiSlot = other;
}

String wifiAnySsid() {
  if (wifiSlotConfigured(wifiSlot)) return wifiSlotSsid(wifiSlot);
  if (wifiSlotConfigured(0))        return wifiSsid;
  if (wifiSlotConfigured(1))        return wifiSsid2;
  return "";
}


// --- Last logged point, across reboots --------------------------------------
//
// One line: epoch,lat,lon. Written after every accepted point, read once at
// boot. Small enough that the extra write per record costs nothing, and it is
// the difference between a reset being invisible and a reset inventing a
// record.
void saveLastPoint() {
  if (!sdReady) return;
  File f = SD.open(LASTPOINT_PATH, FILE_WRITE);   // FILE_WRITE truncates here
  if (!f) return;
  f.println(String(lastPointEpoch) + "," + String(lastPointLat, 6) + "," +
            String(lastPointLon, 6));
  f.close();
}

void loadLastPoint() {
  if (!sdReady || !SD.exists(LASTPOINT_PATH)) return;
  File f = SD.open(LASTPOINT_PATH, FILE_READ);
  if (!f) return;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();

  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return;

  unsigned long ep = (unsigned long)line.substring(0, c1).toInt();
  double la = line.substring(c1 + 1, c2).toDouble();
  double lo = line.substring(c2 + 1).toDouble();
  if (ep == 0 || (la == 0.0 && lo == 0.0)) return;

  lastPointEpoch = ep;
  lastPointLat   = la;
  lastPointLon   = lo;
  hasLoggedPoint = true;
  Serial.println("Last point restored from card (epoch " + String(ep) + ").");
}


// Write, verify, THEN swap.
//
// This used to SD.remove(CONFIG_PATH) and reopen it for writing. Lose power in
// that window - which a unit with no off switch does every time the battery is
// pulled - and there is no config.txt at all. The next boot then wrote defaults
// over the card, so a brownout did not just interrupt a save, it destroyed the
// Wi-Fi password, the AP password, the device id and every tuning value. That
// is what happened on 14 October 2026.
//
// Now the new file is built as config.tmp and only promoted once it is written
// and its size checked, with the previous copy kept as config.bak. The worst a
// power cut can now do is leave a stale config.txt and an orphan config.tmp,
// and loadConfigFromSD() reports both.
bool saveConfigToSD() {
  if (!sdReady) return false;
  SD.remove(CONFIG_TMP);
  File f = SD.open(CONFIG_TMP, FILE_WRITE);
  if (!f) {
    Serial.println("Config save FAILED (could not open config.tmp).");
    return false;
  }
  f.println("# FPE EcoSync Field configuration");
  f.println("# One key=value per line. Lines starting with # are ignored.");
  f.println("");
  f.println("# WHICH UNIT THIS IS. Goes into job_id on every record, and it is");
  f.println("# what tells two units' work apart in ArcGIS. Change it ONLY when");
  f.println("# setting up a new unit - changing it on a unit that has already");
  f.println("# uploaded records splits that unit's history across two names.");
  f.println("# Blank falls back to the value compiled into the firmware.");
  f.println("device_id=" + currentJobID);
  f.println("");
  f.println("# One block per dashboard button. control<N> is written to the");
  f.println("# control_type field in ArcGIS - if that field uses a coded-value");
  f.println("# domain, put the CODE here, not the description.");
  f.println("#");
  f.println("# mode<N> is POINT, CONTINUOUS, POLYGON or LINE, and decides what the");
  f.println("# two tuning numbers mean:");
  f.println("#   POINT       tunea = cooldown (s)       tuneb = movement that releases it (m)");
  f.println("#   CONTINUOUS  tunea = seconds per fix    tuneb = release hold (s)");
  f.println("#               a run also ends after 15 m of movement with the trigger up");
  f.println("#   POLYGON     tunea = vertex spacing (m) tuneb = idle timeout (s)");
  f.println("#   LINE        tunea = vertex spacing (m) tuneb = idle timeout (s), 0 = off");
  for (int i = 0; i < NUM_SLOTS; i++) {
    String n = String(i + 1);
    f.println("target"  + n + "=" + slotWeed[i]);
    f.println("control" + n + "=" + slotControl[i]);
    f.println("mode"    + n + "=" + slotMode[i]);
    f.println("tunea"   + n + "=" + String(slotA[i], 1));
    f.println("tuneb"   + n + "=" + String(slotB[i], 1));
    f.println("");
  }
  f.println("job=" + currentJobNumber);
  f.println("");
  f.println("# Wi-Fi. Two networks, tried in turn, then the setup access point.");
  f.println("# Leave both SSIDs blank to force setup access-point mode.");
  f.println("#");
  f.println("# 1 - Starlink, or whatever network is at the vehicle.");
  f.println("wifi_ssid=" + wifiSsid);
  f.println("wifi_pass=" + wifiPass);
  f.println("#");
  f.println("# 2 - the operator's phone hotspot. Prefer this in the field: the");
  f.println("#     unit joins the phone, so the phone keeps its cellular data");
  f.println("#     and Field Maps still works while you read the dashboard.");
  f.println("#     iPhone: turn on Maximise Compatibility, the unit is 2.4 GHz only.");
  f.println("wifi_ssid2=" + wifiSsid2);
  f.println("wifi_pass2=" + wifiPass2);
  f.println("");
  f.println("# Serve this unit's GNSS position to phone apps (ArcGIS QuickCapture,");
  f.println("# Survey123) over TCP, so they log at the unit's accuracy rather than");
  f.println("# the handset's. In the app choose a Network connection receiver and");
  f.println("# enter this unit's address and port.");
  f.println("cpu_mhz=" + String(cpuMhz));
  f.println("ui_refresh=" + String(uiRefreshS));
  f.println("wifi_sleep=" + String(wifiSleep ? "1" : "0"));
  f.println("nmea_out=" + String(nmeaOut ? "1" : "0"));
  f.println("nmea_port=" + String(nmeaPort));
  f.println("");
  f.println("# Transmit power in dBm, 2.0 to 19.5. Raise this if the unit cannot");
  f.println("# reach the router from the vehicle. 19.5 is full power.");
  f.println("wifi_tx_dbm=" + String(wifiTxDbm, 1));
  f.println("");
  f.println("# Fallback access point, used when the Wi-Fi above cannot be joined.");
  f.println("# ap_pass must be at least 8 characters or the AP will be left open.");
  f.println("# 1 keeps the setup access point running at all times, so the config");
  f.println("# page is reachable even when the unit is joined to Wi-Fi. 0 = only as fallback.");
  f.println("# ap_always keeps the setup AP up permanently. It costs about 45 mA,");
  f.println("# roughly 2.25 %/h on a 2000 mAh cell, so leave it off unless you have");
  f.println("# a reason. ap_idle_stop is minutes with no phone attached before the");
  f.println("# AP shuts itself down; 0 disables it.");
  f.println("# Once it has shut down it reopens for ap_window_sec seconds every");
  f.println("# ap_window_min minutes, so there is always a way back in without the");
  f.println("# dashboard. 15 min / 120 s costs about 0.3 %/h. 0 disables the window");
  f.println("# and leaves you with the trigger gesture and a power cycle.");
  f.println("# Holding the trigger for 5 s with the SAFETY ON also brings it up.");
  f.println("ap_always=" + String(apAlways ? "1" : "0"));
  f.println("ap_idle_stop=" + String(apIdleStopMin, 1));
  f.println("ap_window_min=" + String(apWindowMin, 1));
  f.println("ap_window_sec=" + String(apWindowSec, 0));
  f.println("ap_ssid=" + apSsid);
  f.println("ap_pass=" + apPass);
  f.println("");
  f.println("# ArcGIS API key / token. Blank = anonymous (layer must be public).");
  f.println("esri_token=" + esriToken);
  f.println("");
  f.println("# Config-page login. Blank admin_pass disables the prompt.");
  f.println("admin_user=" + adminUser);
  f.println("admin_pass=" + adminPass);
  f.println("");
  f.println("# Reject logging above this horizontal accuracy, in metres.");
  f.println("gps_max_acc=" + String(gpsMaxAccM, 1));
  f.println("# Minimum movement between polygon vertices, in metres.");
  f.println("poly_min_dist=" + String(polyMinVertexM, 1));
  f.println("# Douglas-Peucker tolerance applied before upload, in metres. 0 disables.");
  f.println("poly_simplify=" + String(polySimplifyM, 1));
  f.println("# Seconds of trigger silence that closes an open polygon.");
  f.println("poly_timeout=" + String(polyTimeoutS, 1));
  f.println("");
  f.println("# Point de-duplication. A press is logged when EITHER point_cooldown");
  f.println("# seconds have passed OR the operator has moved point_min_dist metres");
  f.println("# from the last point. Stops one large weed becoming twenty records,");
  f.println("# without losing the next weed if you move straight to it.");
  f.println("# Set point_min_dist=0 for a plain timer.");
  f.println("point_cooldown=" + String(pointCooldownS, 1));
  f.println("point_min_dist=" + String(pointMinDistM, 1));
  f.println("");
  f.println("# How long the pad must be held shut before a press is logged, in");
  f.println("# milliseconds. Raise it if a finger resting on the pad logs points;");
  f.println("# lower it if deliberate presses are being missed. 20-2000, or 0 to");
  f.println("# turn the duration check off and log on the edge.");
  f.println("trigger_hold_ms=" + String(triggerHoldMs));
  f.println("");
  f.println("# Where records are posted. All four have a built-in default, so");
  f.println("# leaving a key out - or blank - keeps the compiled value. Set one");
  f.println("# only to send a unit's records somewhere other than the usual layer.");
  f.println("esri_point_url=" + esriPointServiceURL);
  f.println("esri_poly_url="  + esriPolygonServiceURL);
  f.println("esri_track_url=" + esriTrackServiceURL);
  f.println("esri_line_url="  + esriLineServiceURL);
  f.flush();
  f.close();

  // Did it actually land? A card that fills up or drops out mid-write still
  // closes cleanly, so trust the file's size rather than the return codes.
  File chk = SD.open(CONFIG_TMP, FILE_READ);
  if (!chk) {
    Serial.println("Config save FAILED (config.tmp vanished after writing).");
    return false;
  }
  size_t wrote = chk.size();
  chk.close();
  if (wrote < CONFIG_MIN_BYTES) {
    Serial.println("Config save FAILED (config.tmp only " + String(wrote) +
                   " bytes - keeping the old config).");
    SD.remove(CONFIG_TMP);
    return false;
  }

  // Keep the outgoing copy. If anything goes wrong from here there is still a
  // complete configuration on the card.
  SD.remove(CONFIG_BAK);
  if (SD.exists(CONFIG_PATH)) SD.rename(CONFIG_PATH, CONFIG_BAK);

  if (!SD.rename(CONFIG_TMP, CONFIG_PATH)) {
    Serial.println("Config save FAILED at the rename - restoring the backup.");
    if (SD.exists(CONFIG_BAK)) SD.rename(CONFIG_BAK, CONFIG_PATH);
    return false;
  }
  return true;
}

void loadConfigFromSD() {
  if (!sdReady) {
    Serial.println("No SD card - running on built-in defaults.");
    sanitiseConfig();
    return;
  }

  // An interrupted save leaves this behind. Worth saying out loud - it is the
  // fingerprint of a power cut during a write, which on this hardware means a
  // flat battery or a loose connection rather than anything in software.
  if (SD.exists(CONFIG_TMP)) {
    Serial.println("WARNING: config.tmp found - a previous save was interrupted.");
    configWarning = "A previous settings save was interrupted (config.tmp left "
                    "on the card). Check the battery and connections.";
    SD.remove(CONFIG_TMP);
  }

  if (!SD.exists(CONFIG_PATH)) {
    // Do NOT write defaults here. The old code did, which turned "could not
    // read the config" into "destroyed the config" - the file was gone, so
    // saving defaults made the loss permanent.
    if (SD.exists(CONFIG_BAK)) {
      Serial.println("config.txt missing - restoring from config.bak.");
      if (SD.rename(CONFIG_BAK, CONFIG_PATH)) {
        configWarning = "config.txt was missing and has been restored from the "
                        "backup. Check your settings are right.";
      } else {
        Serial.println("Restore FAILED - running on built-in defaults.");
        configWarning = "config.txt is missing and the backup could not be "
                        "restored. Running on built-in defaults - NOTHING is "
                        "configured for this unit.";
        sanitiseConfig();
        return;
      }
    } else {
      Serial.println("No config.txt and no backup - running on built-in "
                     "defaults. Nothing has been written to the card.");
      configWarning = "No config.txt on the card. Running on built-in defaults "
                      "- set this unit up before working.";
      sanitiseConfig();
      return;
    }
  }

  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (!f) { sanitiseConfig(); return; }

  // Streamed, not buffered. An earlier version read a fixed 40 lines into an
  // array, which silently dropped the last settings once the commented config
  // file grew past that - a setting you saved simply would not take effect.
  // Only the legacy path needs to hold anything, and that is four lines.
  bool decided = false;
  bool legacy  = false;
  String legacyLines[4];
  int legacyCount = 0;

  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();

    if (!decided) {
      // Skip blanks and comments until the first line that tells us the format.
      if (l.length() == 0 || l.charAt(0) == '#') continue;
      legacy  = (l.indexOf('=') < 0);   // old format was four bare lines
      decided = true;
    }

    if (legacy) {
      if (l.length() > 0 && legacyCount < 4) legacyLines[legacyCount++] = l;
    } else {
      applyConfigLine(l);
    }
  }
  f.close();

  if (legacy) {
    if (legacyCount > 0) slotWeed[0]      = legacyLines[0];
    if (legacyCount > 1) slotWeed[1]      = legacyLines[1];
    if (legacyCount > 2) slotWeed[2]      = legacyLines[2];
    if (legacyCount > 3) currentJobNumber = legacyLines[3];
    // control1..3 and the timings are new; sanitiseConfig seeds the defaults.
    sanitiseConfig();
    Serial.println("Legacy config.txt detected - migrated to key=value format.");
    saveConfigToSD();
  } else {
    sanitiseConfig();
    Serial.println("Config loaded from SD.");
  }
}

// Load the TLS root certificate, if one has been placed on the card.
void loadCaCertFromSD() {
  caCert = "";
  if (!sdReady || !SD.exists(CA_CERT_PATH)) {
    Serial.println("WARNING: no /ca.pem on SD - TLS certificate checking is DISABLED.");
    return;
  }
  File f = SD.open(CA_CERT_PATH, FILE_READ);
  if (!f) return;
  caCert.reserve(f.size() + 8);
  while (f.available()) caCert += (char)f.read();
  f.close();
  if (caCert.indexOf("BEGIN CERTIFICATE") < 0) {
    Serial.println("WARNING: /ca.pem does not look like a PEM certificate - ignoring.");
    caCert = "";
    return;
  }
  Serial.println("TLS root certificate loaded from SD (" + String(caCert.length()) + " bytes).");
}

// ==========================================
// 6. SD RECORD FUNCTIONS
// ==========================================
int countRecords(const char *path) {
  if (!sdReady || !SD.exists(path)) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length() > 0) count++;
  }
  f.close();
  return count;
}

// Recover the true backlog after a power cycle. Without this the dashboard
// reports zero unsynced records while data is still sitting on the card.
void recoverUnsyncedCounts() {
  unsyncedPoints   = countRecords(PENDING_PTS);
  unsyncedPolygons = countRecords(PENDING_POLY);
  unsyncedTrack    = countRecords(PENDING_TRACK);
  unsyncedLines    = countRecords(PENDING_LINE);
  if (unsyncedPoints || unsyncedPolygons || unsyncedTrack || unsyncedLines) {
    Serial.println("Recovered backlog: " + String(unsyncedPoints) + " points, " +
                   String(unsyncedTrack) + " track, " +
                   String(unsyncedPolygons) + " polygons, " +
                   String(unsyncedLines) + " lines pending sync.");
    lastSyncStatus = String(unsyncedPoints) + " pts / " + String(unsyncedTrack) +
                     " trk / " + String(unsyncedPolygons) + " polys / " +
                     String(unsyncedLines) + " lines awaiting sync";
  }
}

// runId groups the points from one trigger squeeze; captureMode says how the
// record was made (POINT, CONTINUOUS, POLY_SHORT). Both are appended at the END
// of the line, so a card holding records written by earlier firmware still
// parses - buildPointFeature simply finds fewer fields and omits the
// attributes, exactly as it already does for control_type and accuracy.
bool saveRecordLocally(String weed, double lat, double lon, unsigned long epochTime,
                       String controlType, double accuracyM,
                       unsigned long runId, const char *captureMode) {
  if (!sdReady) return false;
  // Routed by capture mode, not split later at sync time: a continuous run and
  // a spot record are different kinds of evidence and go to different layers,
  // so they are kept apart from the moment they are written. It also means a
  // track sync failing cannot hold up the spot records.
  bool isTrack = (strcmp(captureMode, "CONTINUOUS") == 0);
  File dataFile = SD.open(isTrack ? PENDING_TRACK : PENDING_PTS, FILE_APPEND);
  if (!dataFile) return false;
  // Commas are stripped so the CSV cannot be split incorrectly.
  weed.replace(",", " ");
  controlType.replace(",", " ");
  String job = currentJobNumber; job.replace(",", " ");
  dataFile.println(weed + "," + String(lat, 6) + "," + String(lon, 6) + "," +
                   String(epochTime) + "," + job + "," + controlType + "," +
                   String(accuracyM, 2) + "," +
                   String(runId) + "," + String(captureMode));
  dataFile.close();
  if (isTrack) unsyncedTrack++;
  else         unsyncedPoints++;
  return true;
}

bool savePolygonLocally(String weed, String coords, unsigned long epochTime,
                        String controlType, double accuracyM) {
  if (!sdReady) return false;
  File dataFile = SD.open(PENDING_POLY, FILE_APPEND);
  if (!dataFile) return false;
  weed.replace("|", " ");
  controlType.replace("|", " ");
  String job = currentJobNumber; job.replace("|", " ");
  dataFile.println(weed + "|" + coords + "|" + String(epochTime) + "|" + job +
                   "|" + controlType + "|" + String(accuracyM, 2));
  dataFile.close();
  unsyncedPolygons++;
  return true;
}

// Same record format as a polygon - the only difference is which queue it
// joins and, later, whether the geometry is closed.
bool saveLineLocally(String weed, String coords, unsigned long epochTime,
                     String controlType, double accuracyM) {
  if (!sdReady) return false;
  File dataFile = SD.open(PENDING_LINE, FILE_APPEND);
  if (!dataFile) return false;
  weed.replace("|", " ");
  controlType.replace("|", " ");
  String job = currentJobNumber; job.replace("|", " ");
  dataFile.println(weed + "|" + coords + "|" + String(epochTime) + "|" + job +
                   "|" + controlType + "|" + String(accuracyM, 2));
  dataFile.close();
  unsyncedLines++;
  return true;
}

// ==========================================
// 7. SENSOR POLLING (the only places that touch I2C)
// ==========================================

// With setAutoPVT(true) the module pushes NAV-PVT on its own schedule and
// getPVT() simply reports whether fresh data has arrived. Nothing blocks.
void pollGPS() {
  static unsigned long lastPoll = 0;
  unsigned long now = millis();
  if (now - lastPoll < gpsPollInterval) return;
  lastPoll = now;

  if (!myGNSS.getPVT()) return;   // no new solution since last check

  live_lat        = myGNSS.getLatitude()  / 10000000.0;
  live_lon        = myGNSS.getLongitude() / 10000000.0;
  live_accuracy_m = myGNSS.getHorizontalAccEst() / 1000.0;
  live_gps_lock   = myGNSS.getGnssFixOk();
  live_siv        = myGNSS.getSIV();
  live_fix_type   = myGNSS.getFixType();

  // Read from the same cached PVT solution as everything above, so this costs
  // no extra bus traffic and cannot block the loop.
  live_diff_soln  = myGNSS.getDiffSoln();
  if (live_diff_soln && !diffSolnEverSeen) {
    diffSolnEverSeen = true;
    diffSolnFirstMs  = now;
    Serial.println("SBAS corrections now being applied (" +
                   String(now / 1000) + "s after boot).");
  }

  unsigned long epoch = myGNSS.getUnixEpoch();
  // Anything before 2020 means the module has not solved time yet.
  live_time_valid = (epoch > 1577836800UL);
  live_epoch      = live_time_valid ? epoch : 0;

  lastGpsUpdate = now;
}

void pollBattery() {
  static unsigned long lastPoll = 0;
  unsigned long now = millis();
  if (!batteryOk) return;
  if (lastPoll != 0 && now - lastPoll < batteryPollInterval) return;
  lastPoll = now;

  float pct = lipo.getSOC();
  if (pct > 100.0) pct = 100.0;
  if (pct < 0.0)   pct = 0.0;
  live_battery_pct = (int)pct;
  live_charge_rate = lipo.getChangeRate();
}

// ==========================================
// 8. POLYGON LIFECYCLE
// ==========================================
// ==========================================
// POLYGON CRASH JOURNAL
// ==========================================
// Vertices lived in RAM until the polygon closed, so a reset part-way through a
// treatment lost the ENTIRE ring - not one point, everything since the trigger
// was first pulled. That was the last unprotected data-loss path in the unit.
//
// Every vertex is now appended to the card as it is captured, and an unfinished
// ring is recovered on the next boot.
//
// One open/append/close per vertex is deliberate. Holding the file open would be
// faster, but unflushed data is exactly what a brownout throws away, which would
// defeat the whole purpose. At a 2 m vertex spacing and walking pace that is a
// write every couple of seconds - well within budget.

String polyRecoveryNote = "";   // set at boot when an unfinished ring was found

void journalStart(const String &weed, const String &control,
                  const String &job, unsigned long epoch, bool isLine) {
  if (!sdReady) return;
  SD.remove(POLY_JOURNAL);                    // never append to a stale ring
  File f = SD.open(POLY_JOURNAL, FILE_WRITE);
  if (!f) return;
  String w = weed;    w.replace("|", " ");
  String c = control; c.replace("|", " ");
  String j = job;     j.replace("|", " ");
  // The trailing shape tag was added on 28 Oct. A header without it is a
  // polygon, which is what every journal written before then was.
  f.println("H|" + w + "|" + c + "|" + j + "|" + String(epoch) +
            "|" + String(isLine ? "LINE" : "POLY"));
  f.close();
}

void journalVertex(double lat, double lon, double accM) {
  if (!sdReady) return;
  File f = SD.open(POLY_JOURNAL, FILE_APPEND);
  if (!f) return;
  f.println("V|" + String(lon, 6) + "," + String(lat, 6) + "|" + String(accM, 2));
  f.close();
}

void journalClear() {
  if (!sdReady) return;
  SD.remove(POLY_JOURNAL);
}

// Runs once at boot, after the unsynced counts have been read from the card.
// A ring of three or more vertices goes through the normal pending-polygon path
// so it syncs to ESRI like any other; anything shorter is reported and dropped,
// because two points are not a polygon.
void recoverPolygonJournal() {
  if (!sdReady) return;
  if (!SD.exists(POLY_JOURNAL)) return;

  File f = SD.open(POLY_JOURNAL, FILE_READ);
  if (!f) { polyRecoveryNote = "journal found but could not be opened"; return; }

  String weed, control, job, coords;
  unsigned long epoch   = 0;
  double        worstAcc = 0.0;
  int           verts    = 0;
  bool          haveHeader = false;
  bool          wasLine    = false;
  double        firstLat = 0.0, firstLon = 0.0;   // for the short-ring fallback

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;

    if (line.startsWith("H|")) {
      int p1 = line.indexOf('|', 2);
      int p2 = (p1 < 0) ? -1 : line.indexOf('|', p1 + 1);
      int p3 = (p2 < 0) ? -1 : line.indexOf('|', p2 + 1);
      if (p1 < 0 || p2 < 0 || p3 < 0) continue;      // malformed, ignore
      weed       = line.substring(2, p1);
      control    = line.substring(p1 + 1, p2);
      job        = line.substring(p2 + 1, p3);
      int p4     = line.indexOf('|', p3 + 1);
      if (p4 < 0) {
        epoch   = strtoul(line.substring(p3 + 1).c_str(), NULL, 10);
        wasLine = false;                      // pre-28-Oct journal: a ring
      } else {
        epoch   = strtoul(line.substring(p3 + 1, p4).c_str(), NULL, 10);
        wasLine = (line.substring(p4 + 1) == "LINE");
      }
      haveHeader = true;
    } else if (line.startsWith("V|")) {
      // A vertex line can be truncated mid-write by the power loss that caused
      // the recovery in the first place. Validate before trusting it.
      int p1 = line.indexOf('|', 2);
      if (p1 < 0) continue;
      String pair = line.substring(2, p1);
      if (pair.indexOf(',') < 0) continue;
      double a = line.substring(p1 + 1).toFloat();
      if (verts == 0) {
        int comma = pair.indexOf(',');
        firstLon  = pair.substring(0, comma).toDouble();
        firstLat  = pair.substring(comma + 1).toDouble();
      }
      coords += pair + ";";
      verts++;
      if (a > worstAcc) worstAcc = a;
    }
  }
  f.close();

  if (haveHeader && wasLine) {
    String savedJob  = currentJobNumber;
    currentJobNumber = job;
    if (verts >= 2) {
      bool ok = saveLineLocally(weed, coords, epoch, control, worstAcc);
      polyRecoveryNote = ok
        ? ("RECOVERED an unfinished line: " + String(verts) + " vertices (saved, will sync)")
        : ("Found an unfinished line of " + String(verts) +
           " vertices but the SAVE FAILED - check the card");
    } else if (verts == 1) {
      bool ok = saveRecordLocally(weed, firstLat, firstLon, epoch, control, worstAcc,
                                  epoch, "LINE_SHORT");
      polyRecoveryNote = ok
        ? "RECOVERED an unfinished line of 1 vertex - saved as a POINT"
        : "Found an unfinished line of 1 vertex but the SAVE FAILED - check the card";
    } else {
      polyRecoveryNote = "Unfinished line had no usable vertices - discarded";
    }
    currentJobNumber = savedJob;
    Serial.println(polyRecoveryNote);
    SD.remove(POLY_JOURNAL);
    return;
  }

  if (haveHeader && verts >= 3) {
    // savePolygonLocally reads the job number from the live global, so pin it to
    // the one that was current when the ring started, then put it back.
    String savedJob  = currentJobNumber;
    currentJobNumber = job;
    bool ok = savePolygonLocally(weed, coords, epoch, control, worstAcc);
    currentJobNumber = savedJob;

    polyRecoveryNote = ok
      ? ("RECOVERED an unfinished polygon: " + String(verts) + " vertices, " +
         weed + " (saved, will sync)")
      : ("Found an unfinished polygon of " + String(verts) +
         " vertices but the SAVE FAILED - check the card");
  } else if (haveHeader && verts >= 1) {
    // Same reasoning as closePolygon: too short to be a ring, but a spray did
    // happen and the record has to survive the power loss that got us here.
    String savedJob  = currentJobNumber;
    currentJobNumber = job;
    bool ok = saveRecordLocally(weed, firstLat, firstLon, epoch, control, worstAcc,
                                epoch, "POLY_SHORT");
    currentJobNumber = savedJob;

    if (ok) polyAsPoint++;
    polyRecoveryNote = ok
      ? ("RECOVERED an unfinished polygon of " + String(verts) +
         " vertices - too few for a ring, saved as a POINT")
      : ("Found an unfinished polygon of " + String(verts) +
         " vertices but the SAVE FAILED - check the card");
  } else if (haveHeader) {
    polyDiscarded++;
    polyRecoveryNote = "Unfinished polygon had no usable vertices - discarded";
  } else {
    polyRecoveryNote = "Journal file was present but unreadable - discarded";
  }

  Serial.println(polyRecoveryNote);
  SD.remove(POLY_JOURNAL);
}

// Copy the active slot's mode and tuning into the live globals the capture code
// already reads. Keeping those globals means the polygon sampler, the point
// de-duplication and the read-only settings page are all untouched by the move
// to slots - they carry on reading the same variables they always did.
void applySlotTuning() {
  loggingMode = slotMode[currentSlot];
  if (loggingMode == "POINT") {
    pointCooldownS = slotA[currentSlot];
    pointMinDistM  = slotB[currentSlot];
  } else if (loggingMode == "POLYGON" || loggingMode == "LINE") {
    // LINE shares the polygon tuning: A is vertex spacing, B is the idle
    // timeout. Without this branch a LINE slot's tuning was never applied at
    // all, so a line ran on whatever the last polygon slot happened to leave
    // behind - including a 15 s timeout that would close it mid-trail.
    polyMinVertexM = slotA[currentSlot];
    polyTimeoutS   = slotB[currentSlot];
  }
  // CONTINUOUS reads slotA/slotB directly - there is no older global to honour.
}

// A continuous run ends when the trigger is released, the target changes, the
// safety engages or the mode changes. Nothing is written here: every point was
// already on the card the moment it was taken, which is the entire reason for
// capturing this way.
void endContinuousRun(const char *reason) {
  if (currentRunId == 0) return;
  continuousRuns++;
  Serial.println("--- RUN ENDED (" + String(reason) + "): " +
                 String(continuousPoints) + " points, id " + String(currentRunId) + " ---");
  currentRunId     = 0;
  continuousPoints = 0;
  runReleasedAt    = 0;
}

void startPolygonAt(double lat, double lon, unsigned long epoch) {
  polygonActive        = true;
  shapeIsLine          = (loggingMode == "LINE");
  trailSegNow          = ++trailSegLast;
  currentPolygonCoords = "";
  currentPolygonCoords.reserve(POLY_COORD_RESERVE);
  currentPolygonWeed    = currentWeed;          // pin the target at start time
  currentPolygonControl = currentControlType;   // and its control method
  vertexCount          = 0;
  polyStartTime        = epoch;
  lastPolySampleTime   = millis();
  polyWorstAccM        = live_accuracy_m;

  currentPolygonCoords += String(lon, 6) + "," + String(lat, 6) + ";";
  trailAdd(lat, lon, trailSegNow);
  vertexCount   = 1;
  lastVertexLat = lat;
  lastVertexLon = lon;
  polyStartLat  = lat;
  polyStartLon  = lon;

  // The ring is now crash-safe from its very first vertex.
  journalStart(currentPolygonWeed, currentPolygonControl, currentJobNumber,
               epoch, shapeIsLine);
  journalVertex(lat, lon, polyWorstAccM);
}

// allowSave=false discards; otherwise a polygon of 3+ vertices is written to SD.
void closePolygon(const char *reason, bool allowSave) {
  if (!polygonActive) return;
  polygonActive = false;

  // A line needs two vertices, not three, and is never closed into a ring.
  // Below two there is no line at all, so it degrades to a point the same way
  // a short ring does - a capture must never leave no trace.
  if (shapeIsLine) {
    if (allowSave && vertexCount >= 2) {
      if (saveLineLocally(currentPolygonWeed, currentPolygonCoords, polyStartTime,
                          currentPolygonControl, polyWorstAccM)) {
        lastAction = "Saved line, " + String(vertexCount) + " pts (" + reason + ")";
      } else {
        lastAction = "LINE SAVE FAILED - check SD card";
      }
    }
    else if (allowSave && vertexCount >= 1) {
      if (saveRecordLocally(currentPolygonWeed, polyStartLat, polyStartLon, polyStartTime,
                            currentPolygonControl, polyWorstAccM,
                            polyStartTime, "LINE_SHORT")) {
        lastAction = "Too short for a line (" + String(vertexCount) +
                     " pts) - saved as a POINT (" + String(reason) + ")";
      } else {
        lastAction = "SHORT LINE SAVE FAILED - check SD card";
      }
    }
    else {
      lastAction = "Line discarded, no vertices (" + String(reason) + ")";
    }
    Serial.println("--- LINE CLOSED: " + String(reason) +
                   " (" + String(vertexCount) + " vertices) ---");
    journalClear();
    currentPolygonCoords = "";
    vertexCount = 0;
    shapeIsLine = false;
    trailSegNow = 0;
    return;
  }

  if (allowSave && vertexCount >= 3) {
    if (savePolygonLocally(currentPolygonWeed, currentPolygonCoords, polyStartTime,
                           currentPolygonControl, polyWorstAccM)) {
      polySaved++;
      lastAction = "Saved polygon, " + String(vertexCount) + " pts (" + reason + ")";
    } else {
      lastAction = "POLYGON SAVE FAILED - check SD card";
    }
  }
  // Fewer than three vertices is not a ring - but the operator still sprayed.
  //
  // Vertices are considered once a second, so a two-second squeeze on a small
  // weed produces two of them and the whole event used to be dropped in
  // silence. On a block of ten small patches worked in quick succession that
  // was most of the day's records gone, with nothing on the SD card to show
  // for it. A spray event must never leave no trace, so degrade to a point
  // rather than discarding: it is the same information the POINT mode would
  // have captured, and it is a defensible record.
  else if (allowSave && vertexCount >= 1) {
    if (saveRecordLocally(currentPolygonWeed, polyStartLat, polyStartLon, polyStartTime,
                          currentPolygonControl, polyWorstAccM,
                          polyStartTime, "POLY_SHORT")) {
      polyAsPoint++;
      lastAction = "Too short for a polygon (" + String(vertexCount) +
                   " pts) - saved as a POINT (" + String(reason) + ")";
    } else {
      lastAction = "SHORT POLYGON SAVE FAILED - check SD card";
    }
  }
  else {
    polyDiscarded++;
    lastAction = "Polygon discarded, " + String(vertexCount) + " pts (" + reason + ")";
  }
  Serial.println("--- POLYGON CLOSED: " + String(reason) +
                 " (" + String(vertexCount) + " vertices) ---");

  // The ring is either safely in pending_poly.csv or deliberately discarded.
  // Either way the journal has done its job and must not survive to be
  // recovered as a phantom polygon on the next boot.
  journalClear();

  currentPolygonCoords = "";
  vertexCount = 0;
  trailSegNow = 0;
}

// Distance-gated vertex capture. Standing still adds nothing, so heap use is
// bounded by ground covered rather than by time spent with the trigger down.
void samplePolygonVertex() {
  if (!polygonActive) return;

  unsigned long now = millis();
  if (now - lastPolySampleTime < polySampleInterval) return;
  lastPolySampleTime = now;

  if (!gpsUsable()) return;

  double lat = live_lat;
  double lon = live_lon;

  if (metresBetween(lastVertexLat, lastVertexLon, lat, lon) < polyMinVertexM) return;

  // Backstop: split into a new polygon rather than growing without limit.
  if (vertexCount >= MAX_POLY_VERTICES) {
    Serial.println("Vertex cap reached - splitting polygon.");
    closePolygon("vertex limit", true);
    startPolygonAt(lastVertexLat, lastVertexLon,
                   live_time_valid ? live_epoch : polyStartTime);
    lastTriggerActivity = now;   // do not let the split trip the idle timeout
  }

  currentPolygonCoords += String(lon, 6) + "," + String(lat, 6) + ";";
  trailAdd(lat, lon, trailSegNow);
  vertexCount++;
  lastVertexLat = lat;
  lastVertexLon = lon;

  // Keep the WORST figure, so the stored value is a ceiling for the whole ring.
  if (live_accuracy_m > polyWorstAccM) polyWorstAccM = live_accuracy_m;

  journalVertex(lat, lon, live_accuracy_m);
}

// ==========================================
// 9. NETWORKING
// ==========================================
String wifiLastError = "not attempted yet";
int    wifiLastStatus = -1;

// Turn the raw status code into something an operator can act on. "network not
// found" and "wrong password" need completely different responses, and without
// this the dashboard just says "disconnected" for both.
const char *wifiStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "network NOT FOUND (wrong name, or 5GHz-only)";
    case WL_SCAN_COMPLETED:  return "scan completed";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "REJECTED (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "unknown";
  }
}

// AP_STA, not AP. Running both means the config page stays reachable while the
// station side keeps trying the real network in the background - so a unit that
// booted before the dish came up can still join later without a power cycle.
void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(apSsid.c_str(), apPass.length() >= 8 ? apPass.c_str() : NULL);
  apMode = true;
  apSuppressed = false;
  apLastClientMs = millis();   // grace period before the idle shutdown can fire
  if (ok) Serial.println("Setup access point up: " + apSsid + " at " + WiFi.softAPIP().toString());
  else    Serial.println("Failed to start access point.");
}

// Take the AP down and return the radio to plain station mode, which is what
// re-enables modem sleep.
void stopAccessPoint(const char *why) {
  if (!apMode) return;
  WiFi.softAPdisconnect(true);
  apMode = false;
  apStoppedAt = millis();
  if (apStoppedAt == 0) apStoppedAt = 1;   // 0 means "never stopped"
  apWindowStartedAt = 0;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(wifiSleep);
  Serial.println("Access point stopped (" + String(why) + ").");
}

// Returns true if joined as a station. Falls back to access-point mode so the
// config page is always reachable, even with no credentials on the card.
bool connectWiFi(unsigned long timeoutMs) {
  if (!wifiSlotConfigured(0) && !wifiSlotConfigured(1)) {
    Serial.println("No Wi-Fi SSID configured in either slot.");
    wifiLastError = "no SSID configured";
    startAccessPoint();
    return false;
  }

  // A blocking attempt counts as an attempt, so the background retry does not
  // fire again the moment this one gives up.
  lastWifiAttempt = millis();
  wifiRetryPending = false;

  // Same reason as the retry in maintainWiFi(): if a join is already in flight
  // - which it is whenever this is called for new credentials rather than at
  // boot - esp_wifi refuses the new config and the begin() below is ignored.
  WiFi.disconnect(false, false);

  // Keep the AP alive if it is already up; otherwise station-only.
  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  // Modem sleep parks the radio between beacons. It is only available when NOT
  // running an access point - an AP must stay awake to beacon, which is most of
  // why ap_always costs ~45 mA.
  WiFi.setSleep(wifiSleep && !apMode);
  WiFi.setTxPower(txPowerSetting());
  if (!wifiSlotConfigured(wifiSlot)) wifiNextSlot();
  Serial.println("Wi-Fi: trying " + wifiSlotLabel(wifiSlot) + " (" +
                 wifiSlotSsid(wifiSlot) + ")");
  WiFi.begin(wifiSlotSsid(wifiSlot).c_str(), wifiSlotPass(wifiSlot).c_str());

  Serial.print("Joining " + wifiSsid + " ");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  wifiLastStatus = WiFi.status();
  if (wifiLastStatus == WL_CONNECTED) {
    wifiLastError = "";
    Serial.println("Wi-Fi connected: " + WiFi.localIP().toString() +
                   "  RSSI " + String(WiFi.RSSI()) + " dBm");
    return true;
  }

  wifiLastError = String(wifiStatusName(wifiLastStatus));
  Serial.println("Wi-Fi join FAILED: " + wifiLastError);
  if (!apMode) startAccessPoint();
  return false;
}

// Retries in BOTH modes. The previous version gave up permanently once it had
// fallen back to the access point, so a unit powered on before Starlink had
// service stayed offline for the whole day.
void maintainWiFi() {
  unsigned long now = millis();

  // --- Idle shutdown, whatever the station side is doing ---------------------
  //
  // Checked FIRST and unconditionally, because the expensive case is precisely
  // the one where the station never comes back: out of Starlink range the AP
  // used to stay up all day for nobody, at about 45 mA. Drain went from 2.75
  // to 5.4 %/h the day that shipped.
  //
  // A client attached means someone is using it, so the clock restarts.
  if (apMode && !apAlways) {
    if (WiFi.softAPgetStationNum() > 0) {
      apLastClientMs = now;
      apWindowStartedAt = 0;      // somebody joined: this is no longer a peek
    } else if (apWindowStartedAt != 0) {
      if (now - apWindowStartedAt > (unsigned long)(apWindowSec * 1000.0)) {
        stopAccessPoint("window closed, nobody joined");
        apSuppressed = true;
      }
    } else if (apIdleStopMin > 0.0 &&
               now - apLastClientMs > (unsigned long)(apIdleStopMin * 60000.0)) {
      stopAccessPoint("idle");
      // Do not let the fallback below bring it straight back - the window
      // below is what reopens it, on a clock.
      apSuppressed = true;
    }
  }

  // --- Periodic window -------------------------------------------------------
  //
  // Runs BEFORE the connected check on purpose. The case that needs it most is
  // not "out of range" - it is the unit sitting happily on Starlink while the
  // phone is on mobile data, where nothing is down and nothing would otherwise
  // bring the AP back.
  if (!apMode && !apAlways && apWindowMin > 0.0 && apStoppedAt != 0 &&
      now - apStoppedAt > (unsigned long)(apWindowMin * 60000.0)) {
    Serial.println("Access point window opening for " + String(apWindowSec, 0) + "s.");
    startAccessPoint();                    // clears apSuppressed, sets apLastClientMs
    apWindowStartedAt = millis();
    if (apWindowStartedAt == 0) apWindowStartedAt = 1;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    // Station side is back and nobody is on the AP - no reason to keep it.
    // Unless a window is open, which would otherwise be closed the instant it
    // opened whenever the unit had a link.
    if (apMode && !apAlways && apWindowStartedAt == 0 &&
        WiFi.softAPgetStationNum() == 0) {
      stopAccessPoint("station link restored");
    }
    return;
  }

  if (!wifiSlotConfigured(0) && !wifiSlotConfigured(1)) return;

  if (wifiDownSince == 0) {
    wifiDownSince = now;
    // The link has just gone down. Whatever suppression an earlier idle
    // shutdown left behind is stale - losing the network is a new reason to
    // want the AP, and waiting up to a whole window for it would be worse than
    // the behaviour this replaced. Cleared only on the transition, so a unit
    // that is out of range all day still idles its AP down and stays down.
    apSuppressed = false;
  }

  // Start the access point once the station side has been down for a while.
  //
  // THIS is what stranded the unit in the field. The AP was only ever started
  // from connectWiFi(), which runs at boot. Switch on inside Starlink range and
  // the join succeeds, so no AP is started; drive out of range and this
  // function retried a network that was not there, every 30 seconds, forever -
  // leaving no interface to connect to at all. The reverse case (booted out of
  // range, AP up, network appears later) was fixed long ago; this direction
  // never was.
  if (!apMode && !apSuppressed && now - wifiDownSince > apFallbackMs) {
    Serial.println("Station down " + String((now - wifiDownSince) / 1000) +
                   "s - starting fallback access point.");
    startAccessPoint();
  }

  if (now - lastWifiAttempt < wifiRetryInterval) return;
  lastWifiAttempt = now;

  wifiLastStatus = WiFi.status();
  wifiLastError  = String(wifiStatusName(wifiLastStatus));

  // The retry has to DISCONNECT before it can begin again.
  //
  // esp_wifi keeps auto-reconnecting on its own, so the station is almost
  // always mid-attempt when this fires. WiFi.begin() then calls
  // esp_wifi_set_config(), the driver refuses it -
  //
  //     E (79892) wifi:sta is connecting, cannot set config
  //
  // - and the retry does nothing at all. Every 30 seconds, forever. The log
  // said "retrying Jimmys" each time and never actually retried anything.
  //
  // Worse, it takes the access point down with it. AP_STA shares one radio, so
  // the softAP follows the station's channel; a station stuck in a permanent
  // connect-and-scan cycle drags the radio off-channel often enough that the
  // AP's beacons go missing and the SSID never appears in anyone's Wi-Fi list.
  // That is why the unit was unreachable BOTH ways at the house.
  //
  // Two passes: drop the attempt, then begin a second later once the
  // disconnect has actually landed.
  if (!apMode) WiFi.mode(WIFI_STA);

  if (!wifiRetryPending) {
    WiFi.disconnect(false, false);        // STA only; leaves the AP alone
    wifiRetryPending = true;
    lastWifiAttempt  = now - wifiRetryInterval + 1000;   // come back in 1 s
    return;
  }
  wifiRetryPending = false;

  wifiJoinAttempts++;
  // Alternate before retrying. Sitting on one SSID that is not present is how
  // an officer with a hotspot up ends up locked out of the dashboard.
  wifiNextSlot();
  Serial.println("Wi-Fi not connected (" + wifiLastError + ") - retrying " +
                 wifiSlotLabel(wifiSlot) + " " + wifiSlotSsid(wifiSlot) +
                 " [attempt " + String(wifiJoinAttempts) + "]");
  WiFi.setTxPower(txPowerSetting());
  WiFi.begin(wifiSlotSsid(wifiSlot).c_str(), wifiSlotPass(wifiSlot).c_str());
}

// Accept one phone at a time. A newer connection replaces an older one, so a
// dropped app can simply reconnect without the unit needing a restart.
// Configure the receiver for SouthPAN, per Geoscience Australia's published
// u-blox settings. Returns true only if every key was accepted.
bool configureSouthPAN() {
  bool ok = true;
  int failed = 0;

  // SBAS on, L1C/A on - SouthPAN's open service rides the legacy L1 signal.
  if (!myGNSS.setVal8(UBLOX_CFG_SIGNAL_SBAS_ENA, 1))      { ok = false; failed++; }
  if (!myGNSS.setVal8(UBLOX_CFG_SIGNAL_SBAS_L1CA_ENA, 1)) { ok = false; failed++; }

  // THE ONE THAT MATTERS. Without it the corrections arrive and are discarded.
  if (!myGNSS.setVal8(UBLOX_CFG_SBAS_USE_TESTMODE, 1))    { ok = false; failed++; }

  // Actually apply the differential corrections - the point of the exercise.
  if (!myGNSS.setVal8(UBLOX_CFG_SBAS_USE_DIFFCORR, 1))    { ok = false; failed++; }

  // GA specify integrity OFF and ranging OFF: SouthPAN is not certified for
  // safety-of-life, and using a GEO as a ranging source can hurt more than help.
  if (!myGNSS.setVal8(UBLOX_CFG_SBAS_USE_INTEGRITY, 0))   { ok = false; failed++; }
  if (!myGNSS.setVal8(UBLOX_CFG_SBAS_USE_RANGING, 0))     { ok = false; failed++; }

  // Search PRN 122 only - SouthPAN on Inmarsat-4F1 at 143.5E. The mask is a
  // bitmask over PRN 120+n, so bit 2 = value 4.
  if (!myGNSS.setVal64(UBLOX_CFG_SBAS_PRNSCANMASK, 4))    { ok = false; failed++; }

  sbasConfigOk     = ok;
  sbasConfigDetail = ok ? "all keys accepted"
                        : (String(failed) + " of 7 keys REFUSED");
  Serial.println("SouthPAN config: " + sbasConfigDetail);
  return ok;
}

// Ask the module to emit GST, and remember whether it agreed.
//
// The request is a UBX-CFG-MSG that must be acknowledged. Sending it directly
// after setI2COutput() reconfigures the port underneath the very transaction
// carrying the request, so a single attempt can be lost even on a module that
// supports GST perfectly well. Settle, then retry before believing a refusal.
bool enableGst() {
  gstEnableAttempted = true;
  gstEnableOk = false;
  uint8_t port = (gnssTransport == "UART1") ? COM_PORT_UART1 : COM_PORT_I2C;
  for (int attempt = 0; attempt < 3 && !gstEnableOk; attempt++) {
    delay(100);
    gstEnableOk = myGNSS.enableNMEAMessage(UBX_NMEA_GST, port);
  }
  Serial.println(gstEnableOk
                 ? "GST enabled on the GNSS module."
                 : "GST could NOT be enabled - module refused the request.");
  return gstEnableOk;
}

void maintainNmeaServer() {
  if (!nmeaOut) return;

  if (!nmeaServerStarted) {
    nmeaServer.begin(nmeaPort);
    nmeaServerStarted = true;
    Serial.println("NMEA feed listening on port " + String(nmeaPort));
  }

  if (!nmeaClient || !nmeaClient.connected()) {
    // accept(), not available(). ESP32 core 3.x deprecated WiFiServer::available()
    // and it is scheduled for removal; accept() is the same call under its new name.
    WiFiClient incoming = nmeaServer.accept();
    if (incoming) {
      nmeaClient.stop();
      nmeaClient = incoming;
      nmeaClient.setNoDelay(true);   // send each sentence immediately, no Nagle
      Serial.println("NMEA feed: phone connected.");
    }
  }
}

// Where an operator should point QuickCapture. Both addresses are offered
// because either route works: join the unit's own access point, or sit on the
// same Wi-Fi as the unit and use its network address.
String nmeaFeedAddresses() {
  String t = "";
  if (WiFi.status() == WL_CONNECTED) t += WiFi.localIP().toString() + ":" + String(nmeaPort);
  if (apMode) {
    if (t.length()) t += "  or  ";
    t += WiFi.softAPIP().toString() + ":" + String(nmeaPort);
  }
  if (t.length() == 0) t = "no network";
  return t;
}

String networkStatusText() {
  String t;
  if (WiFi.status() == WL_CONNECTED) {
    t = WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + " dBm)";
    if (apMode) t += " + AP " + apSsid;
  } else if (apMode) {
    t = "AP ONLY - " + apSsid + " / " + WiFi.softAPIP().toString();
  } else {
    t = "Wi-Fi disconnected";
  }
  return t;
}

// ==========================================
// 10. WEB INTERFACE
// ==========================================
bool wifiReconfigured = false;

// ==========================================
// BOOT / RESET DIAGNOSTICS
// ==========================================

String resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on (battery connected or power restored)";
    case ESP_RST_EXT:       return "EXTERNAL PIN - the EN line was pulled low";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "CRASH - firmware exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT - supply voltage dipped";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

// Called first thing in setup(), before anything can reset the reason.
void recordBoot() {
  esp_reset_reason_t r = esp_reset_reason();
  bootResetCode   = (int)r;
  bootResetReason = resetReasonText(r);

  // A clean power-on is expected: the operator switched it on. A software
  // restart is ours. Everything else means something went wrong.
  bootWasUnexpected = !(r == ESP_RST_POWERON || r == ESP_RST_SW);

  // NVS, not RTC memory: RTC survives a reset but NOT a power cycle, and the
  // whole point is to count restarts across both.
  if (bootPrefs.begin("ecosync", false)) {
    bootCount = bootPrefs.getULong("boots", 0) + 1;
    bootPrefs.putULong("boots", bootCount);
    bootPrefs.end();
  }
}

// Appended after the SD card is up, so a fault that happens in the paddock
// with nobody watching still leaves evidence on the card.
void logBootToSD() {
  if (!sdReady) return;
  File f = SD.open("/bootlog.csv", FILE_APPEND);
  if (!f) return;
  f.println(String(bootCount) + "," + String(bootResetCode) + "," +
            bootResetReason + "," + String(FIRMWARE_BUILD));
  f.close();
}

String uptimeText() {
  unsigned long s = millis() / 1000UL;
  unsigned long h = s / 3600UL; s %= 3600UL;
  unsigned long m = s / 60UL;   s %= 60UL;
  String out = "";
  if (h)         out += String(h) + "h ";
  if (h || m)    out += String(m) + "m ";
  out += String(s) + "s";
  return out;
}

// ==========================================
// SPECULATIVE-REQUEST GUARD
// ==========================================
// Browsers speculatively fetch links they think you are about to tap. Safari
// does it, Chrome does it, and neither asks first. Any GET that changes state
// is therefore a live grenade: a prefetch of /toggleSafety silently disarms
// the unit with no user action at all.
//
// The real fix is that state changes belong on POST, and /toggleSafety has
// been moved. This guard covers the handlers still reached by GET.
bool isSpeculativeRequest() {
  String purpose    = server.header("Purpose");      // Safari, older Chrome
  String xPurpose   = server.header("X-Purpose");    // Safari
  String xMoz       = server.header("X-Moz");        // Firefox
  String secPurpose = server.header("Sec-Purpose");  // Chrome speculation rules
  purpose.toLowerCase();  xPurpose.toLowerCase();
  xMoz.toLowerCase();     secPurpose.toLowerCase();

  return purpose.indexOf("prefetch")    >= 0 || purpose.indexOf("preview")   >= 0 ||
         xPurpose.indexOf("prefetch")   >= 0 || xPurpose.indexOf("preview")  >= 0 ||
         xMoz.indexOf("prefetch")       >= 0 ||
         secPurpose.indexOf("prefetch") >= 0 || secPurpose.indexOf("prerender") >= 0;
}

// Returns true when the caller should do nothing further.
bool rejectSpeculative() {
  if (!isSpeculativeRequest()) return false;
  speculativeBlocked++;
  Serial.println("Refused a speculative browser fetch of " + server.uri() +
                 " (total refused: " + String(speculativeBlocked) + ")");
  server.send(204);          // acknowledged, nothing done
  return true;
}

// Config pages only. Arm / target / sync stay one tap so nobody is locked out
// of the safety control in the field.
bool requireAuth() {
  if (adminPass.length() == 0) return true;      // auth disabled
  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  // Cached values only. No I2C, no GPS calls, nothing blocking in here.
  bool  gpsLock    = live_gps_lock;
  float accuracy_m = live_accuracy_m;
  bool  usable     = gpsUsable();

  String bgColor = gpsLock ? "#2ecc71" : "#e74c3c";
  if (!systemArmed) bgColor = "#95a5a6";

  String statusText = gpsLock ? "GPS LOCKED" : "SEARCHING...";
  if (!systemArmed) statusText = "SAFETY ON";
  if (polygonActive) { bgColor = "#d35400";
                       statusText = shapeIsLine ? "RECORDING LINE..." : "MAPPING POLYGON..."; }

  String lockBtnText  = systemArmed ? "ENGAGE SAFETY" : "ARM SYSTEM";
  String lockBtnColor = systemArmed ? "#e74c3c" : "#2ecc71";

  String batColor = (live_battery_pct > 20) ? "#f1c40f" : "#e74c3c";
  String chargingIcon = (live_charge_rate > 0.5) ? "\xE2\x9A\xA1 " : "";
  String batText = batteryOk ? (chargingIcon + String(live_battery_pct) + "%") : "--%";

  String accColor = "#e74c3c";
  if (usable)       accColor = (accuracy_m <= 1.5) ? "#2ecc71" : "#f39c12";
  else if (gpsLock) accColor = "#f39c12";
  String accText = gpsLock ? "&plusmn; " + String(accuracy_m, 2) + " m" : "No Fix";
  // SBAS is the difference between 1.0 m and 1.5 m CEP on this module, so it
  // belongs next to the number it changes, not buried in diagnostics.
  if (gpsLock && live_diff_soln) accText += " SBAS";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  // Every refresh is a full page rebuild plus a Wi-Fi transmit. At 2 s, a phone
  // left open on this page all day was a continuous load for no benefit.
  html += "<meta http-equiv='refresh' content='" + String(uiRefreshS) + "'>";
  html += "<style>body { background-color: " + bgColor + "; color: white; text-align: center; font-family: sans-serif; padding-top: 3vh; font-size: 1.1em; font-weight: bold; }</style>";
  html += "</head><body>";

  html += "<p style='text-align: right; margin: 0 20px 5px 0; color: " + batColor + "; font-size: 0.9em;'>\xF0\x9F\x94\x8B " + batText + "</p>";
  // The unit id, not the access point name.
  //
  // This is the SAME constant the uploader stamps into job_id on every record,
  // so it cannot disagree with the data. An SSID can: it lives on the SD card,
  // and a card copied to a second unit with the right name but the wrong
  // binary flashed would read "002" while posting everything as 001. Shown
  // beside the build for the same reason - between them, a unit that is not
  // what you think it is becomes obvious instead of silent.
  html += "<h3 style='color:black; margin: 0; font-size: 0.9em; letter-spacing: 1px;'>" +
          String(currentJobID) + "</h3>";
  html += "<p style='color:black; margin:0; font-size:0.6em; opacity:0.7;'>" + String(FIRMWARE_BUILD) + "</p>";
  html += "<h1 style='margin-bottom: 5px; margin-top: 5px;'>" + statusText + "</h1>";

  html += "<p style='margin: 0 0 4px 0; font-size: 0.85em; color: white;'>Accuracy: <span style='background-color: " + accColor + "; color: white; padding: 4px 8px; border-radius: 6px; border: 1px solid white;'>" + accText + "</span></p>";
  html += "<p style='margin: 0 0 12px 0; font-size: 0.7em; color: black; font-weight: normal;'>Sats: " + String(live_siv) + " &nbsp; Fix type: " + String(live_fix_type) + " &nbsp; " + (usable ? "LOGGING ENABLED" : "LOGGING BLOCKED") + "</p>";

  // A restart always lands here with the safety ON, because systemArmed starts
  // false. Say so loudly - otherwise it reads as the safety engaging by itself,
  // which is what sent us looking in the wrong place the first time.
  if (bootWasUnexpected && !bootNoticeDismissed) {
    html += "<div style='background:#c0392b; color:white; padding:8px 10px; margin:0 12px 10px 12px; border-radius:8px; font-size:0.7em; font-weight:normal; border:1px solid white;'>";
    html += "<b>UNEXPECTED RESTART</b><br>" + htmlEscape(bootResetReason);
    html += "<br>The safety is always ON after a restart. Tap ARM before spraying.";
    html += "<br><a href='/dismiss' style='color:white; font-size:0.9em; text-decoration:underline;'>Dismiss</a>";
    html += "</div>";
  }

  // The config could not be read, or a previous save was cut short. Without
  // this the unit looks completely normal while running on built-in defaults -
  // wrong device id, wrong AP password, no Wi-Fi - and the first sign is a day
  // of records logged under the wrong unit.
  if (configWarning.length() > 0) {
    html += "<div style='background:#c0392b; color:white; padding:8px 10px; margin:0 12px 10px 12px; border-radius:8px; font-size:0.7em; font-weight:normal; border:1px solid white;'>";
    html += "<b>SETTINGS PROBLEM</b><br>" + htmlEscape(configWarning);
    html += "</div>";
  }

  // POST, not a link. A GET that changes state can be fired by a browser
  // prefetch with no user action - see isSpeculativeRequest().
  html += "<form method='POST' action='/toggleSafety' style='margin:0 0 10px 0;'>";
  html += "<button type='submit' style='background-color:" + lockBtnColor + "; color:white; padding:10px 20px; border-radius:8px; font-size:0.9em; border:2px solid white; font-weight:bold; font-family:inherit; cursor:pointer;'>" + lockBtnText + "</button>";
  html += "</form>";
  html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";

  html += "<h3 style='color:black; margin-bottom: 2px;'>Active Target: <span id='tgtname'>" +
          htmlEscape(currentWeed) + "</span></h3>";
  html += "<h4 style='color:white; margin: 0 0 5px 0; font-size:0.8em;'>Control: " + htmlEscape(currentControlType) + "</h4>";
  html += "<h4 style='color:white; margin-top: 0; margin-bottom: 5px;'>Job Number: " + htmlEscape(currentJobNumber) + "</h4>";
  html += "<h4 style='color:black; margin-top: 0; margin-bottom: 10px;'>Mode: " + loggingMode;
  if (polygonActive) {
    html += " (RECORDING " + String(vertexCount) + " pts)";
    // A line has no idle timeout by default, so the only thing that ends it is
    // the operator. Say so where they are looking, not in a manual.
    if (shapeIsLine) html += "<br><span style='font-size:0.6em;'>Press the trigger again to end this line</span>";
  }
  if (currentRunId)   html += " (RUN: " + String(continuousPoints) + " pts)";
  html += "</h4>";

  // 2x2 grid, and the capture mode lives on the button.
  //
  // There is deliberately no separate POINT/POLYGON toggle any more. Two
  // controls that can disagree about what you are recording is one control too
  // many for a gloved thumb at arm's length - and it was the source of the
  // "switched mode and lost my weed" confusion. Want the same weed captured two
  // ways? Give it two slots.
  html += "<div style='display:flex; flex-wrap:wrap; justify-content:center; margin:5px auto; width:94%;'>";
  for (int i = 0; i < NUM_SLOTS; i++) {
    bool active = (i == currentSlot);
    html += "<a id='t" + String(i + 1) + "' href='/setTarget?w=" + String(i + 1) +
            "' onclick='return pick(event," + String(i + 1) + ",\"" +
            jsEscape(slotWeed[i]) + "\")' style='flex:0 0 44%; background-color:";
    html += active ? "#1abc9c" : "#2980b9";
    html += "; color:white; padding:10px 4px; margin:3px; text-decoration:none; border-radius:8px; border:3px solid ";
    html += active ? "white" : "transparent";
    html += "; font-size:0.9em; font-weight:bold;'>";
    html += htmlEscape(slotWeed[i]);
    html += "<br><span style='font-size:0.72em; font-weight:normal; opacity:0.9;'>" + htmlEscape(slotControl[i]) + "</span>";
    html += "<br><span style='font-size:0.68em; font-weight:normal; opacity:0.75;'>" + slotMode[i] + "</span>";
    html += "</a>";
  }
  html += "</div>";

  // Target switching without a page load. If fetch fails for any reason the
  // handler falls back to the plain link, so a browser that will not run this
  // still works exactly as before - just slowly.
  html += "<script>function pick(e,n,nm){e.preventDefault();"
          "fetch('/setTarget?w='+n+'&q=1').then(function(r){"
          "if(r.status==409){alert('Line recording - press the trigger to end it first.');return;}"
          "if(!r.ok)throw 0;"
          "for(var i=1;i<=" + String(NUM_SLOTS) + ";i++){var b=document.getElementById('t'+i);"
          "if(!b)continue;b.style.backgroundColor=(i==n)?'#1abc9c':'#2980b9';"
          "b.style.borderColor=(i==n)?'white':'transparent';}"
          "var h=document.getElementById('tgtname');if(h)h.textContent=nm;})"
          ".catch(function(){location.href='/setTarget?w='+n;});return false;}</script>";

  html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";
  html += "<p style='font-size: 0.75em; margin:0;'>Unsynced:</p>";
  html += "<h2 style='font-size: 1.7em; margin: 0;'>" + String(unsyncedPoints) + " Pts";
  if (unsyncedTrack) html += " / " + String(unsyncedTrack) + " Trk";
  html += " / " + String(unsyncedPolygons) + " Polys";
  if (unsyncedLines) html += " / " + String(unsyncedLines) + " Lines";
  html += "</h2><br>";
  html += "<a href='/map' style='background-color:#16a085; color:white; padding:12px 25px; text-decoration:none; border-radius:8px; display:inline-block; border:2px solid white; font-size: 0.9em; margin-bottom:8px;'>MAP VIEW</a><br>";
  html += "<a href='/forceSync' style='background-color:#8e44ad; color:white; padding:12px 25px; text-decoration:none; border-radius:8px; display:inline-block; border:2px solid white; font-size: 0.9em;'>SYNC TO ESRI</a><br><br>";

  // Refused pulls must be impossible to miss. The whole product is hands-free,
  // so a message that only appears on a screen nobody looks at is not a warning.
  if (rejectedPulls > 0) {
    html += "<div style='background:#c0392b; color:white; padding:8px; margin:8px auto; "
            "width:86%; border-radius:6px; font-size:0.8em;'>";
    html += "<b>" + String(rejectedPulls) + " trigger pull";
    html += (rejectedPulls == 1 ? " was" : "s were");
    html += " NOT logged</b><br>Last reason: " + htmlEscape(lastRejectReason);
    // Say something TRUE for the reason that actually fired. The old text
    // told you to raise the accuracy limit whatever the cause, which for a
    // stale fix is useless advice - and acting on it only admits worse points.
    if (lastRejectReason.indexOf("stale") >= 0) {
      html += "<br>The fix shown above is the last one that arrived. Nothing "
              "new has reached the unit for 5 s, so position cannot be trusted. "
              "Raising the accuracy limit will NOT help this.";
    } else if (lastRejectReason.indexOf("No GPS fix") >= 0) {
      html += "<br>No fix at all. Check the sky view.";
    } else {
      html += "<br>Raise the accuracy limit under Advanced if this keeps happening.";
    }
    html += "</div>";
  }

  // Polygon outcomes this session. Spraying is hands-free, so the operator
  // cannot watch the screen while it happens - the only way to know whether
  // the settings are working is to be able to count afterwards.
  if (pointsLogged || polySaved || polyAsPoint || polyDiscarded ||
      continuousRuns || continuousTotal || unsyncedPoints || unsyncedPolygons ||
      unsyncedLines) {
    html += "<p style='font-size: 0.75em; color:black; margin:6px 0 0 0;'>Captured this session:</p>";
    html += "<p style='color:black; font-size:0.85em; margin:2px 0 8px 0;'>";
    bool first = true;
    if (pointsLogged) {
      html += "<b>" + String(pointsLogged) + " points</b>";
      first = false;
    }
    if (continuousRuns || continuousTotal) {
      html += String(continuousRuns) + " runs / " + String(continuousTotal) + " pts";
      first = false;
    }
    if (polySaved) {
      if (!first) html += " &middot; ";
      html += String(polySaved) + " polygons";
      first = false;
    }
    if (polyAsPoint) {
      if (!first) html += " &middot; ";
      html += "<b>" + String(polyAsPoint) + " too short, kept as points</b>";
      first = false;
    }
    if (polyDiscarded) {
      if (!first) html += " &middot; ";
      html += "<b style='color:#7b241c;'>" + String(polyDiscarded) + " lost</b>";
      first = false;
    }
    // Nothing is lost while it sits on the card, but an operator finishing a
    // day should be able to see whether it has gone up yet.
    int waiting = unsyncedPoints + unsyncedPolygons + unsyncedLines;
    if (waiting > 0) {
      if (!first) html += " &middot; ";
      html += String(waiting) + " waiting to upload";
    }
    html += "</p>";
  }

  html += "<p style='font-size: 0.75em; color:black; margin:0;'>Last Action:</p>";
  html += "<p style='color:black; font-size: 0.9em; margin:3px 0 8px 0;'>" + htmlEscape(lastAction) + "</p>";

  if (nmeaOut) {
    bool listening = (nmeaClient && nmeaClient.connected());
    html += "<p style='font-size:0.7em; color:black; font-weight:normal; margin:0 0 4px 0;'>";
    html += listening ? "\xF0\x9F\x93\xB1 Phone connected to GNSS feed" : "GNSS feed ready: ";
    if (!listening) html += htmlEscape(nmeaFeedAddresses());
    html += "</p>";
  }
  html += "<p style='font-size: 0.7em; color:black; font-weight:normal; margin:0 0 4px 0;'>";
  html += "Trigger: " + String(digitalRead(TRIGGER_PIN) == LOW ? "<b>PRESSED</b>" : "released");
  html += " &nbsp;|&nbsp; loop " + String(loopLastMs) + "ms (max " + String(loopMaxMs) + "ms)";
  html += " &nbsp;|&nbsp; " + String(systemArmed ? "ARMED" : "<b>SAFETY ON</b>");
  // Not a warning. Arming the safety to spray something you do not want
  // recorded is a normal way to work, so this reports rather than scolds.
  if (pullsWhileDisarmed > 0) {
    html += " &nbsp;|&nbsp; " + String(pullsWhileDisarmed) + " pulls not logged (safety)";
  }
  html += "</p>";

  // Say out loud how long this page will stay reachable. You are reading it on
  // the AP, so "the AP shuts down in 4 minutes" is information you can act on -
  // and if you are inside a two-minute window, you very much want to know.
  if (apMode && !apAlways) {
    html += "<p style='font-size: 0.7em; color:black; font-weight:normal; margin:0 0 4px 0;'>";
    if (apWindowStartedAt != 0) {
      long left = (long)(apWindowSec * 1000.0) - (long)(millis() - apWindowStartedAt);
      html += "Access point <b>window</b>: closes in " + String(left > 0 ? left / 1000 : 0) +
              "s unless you stay connected";
    } else if (apIdleStopMin > 0.0) {
      long left = (long)(apIdleStopMin * 60000.0) - (long)(millis() - apLastClientMs);
      html += "Access point shuts down " + String(left > 0 ? left / 60000 + 1 : 0) +
              " min after the last phone disconnects";
      if (apWindowMin > 0.0) html += ", then reopens every " + String(apWindowMin, 0) + " min";
    }
    html += "</p>";
  }

  // Restarts and battery drain belong where they will actually be seen. The
  // restart count was buried on /wifiscan, which is no use for spotting a unit
  // that rebooted mid-run - and the fuel gauge reports %/h directly, which the
  // firmware read and then threw away on a lightning-bolt icon.
  html += "<p style='font-size: 0.7em; color:black; font-weight:normal; margin:0 0 4px 0;'>";
  html += "Restarts: " + String(bootCount);
  if (bootWasUnexpected) html += " <b>(last: " + htmlEscape(bootResetReason) + ")</b>";
  if (batteryOk) {
    html += " &nbsp;|&nbsp; Battery " + String(live_battery_pct) + "%";
    if (live_charge_rate < -0.05) {
      float hrs = (float)live_battery_pct / (-live_charge_rate);
      html += " (" + String(-live_charge_rate, 1) + " %/h, ~" + String(hrs, 1) + " h left)";
    } else if (live_charge_rate > 0.5) {
      html += " (charging " + String(live_charge_rate, 1) + " %/h)";
    }
  }
  html += " &nbsp;|&nbsp; AP " + String(apMode ? "up" : "off");
  // Self-documenting, because the one time you need the gesture is the one
  // time you cannot read this page.
  if (!apMode) html += " (hold trigger 5s, safety on)";
  html += "</p>";

  html += "<p style='font-size: 0.7em; color:black; font-weight:normal; margin:0 0 10px 0;'>";
  html += "Accuracy: " + (live_accuracy_m > 0.0 ? String(live_accuracy_m, 1) + " m" : String("--"));
  html += " (limit " + String(gpsMaxAccM, 1) + " m) &nbsp;|&nbsp; Network: " + htmlEscape(networkStatusText());
  if (!sdReady) html += " &nbsp;|&nbsp; <span style='color:#c0392b;'>NO SD CARD</span>";
  if (caCert.length() == 0) html += " &nbsp;|&nbsp; TLS unverified";
  // An unprotected Advanced page exposes the API key and Wi-Fi password to
  // anyone on this network. Never let that state be silent.
  if (adminPass.length() == 0) {
    html += " &nbsp;|&nbsp; <span style='color:#c0392b; font-weight:bold;'>ADVANCED UNLOCKED</span>";
  }
  html += "</p>";

  html += "<a href='/config' style='color:white; font-size:1.2em; text-decoration:underline;'>\xE2\x9A\x99\xEF\xB8\x8F Open Device Configuration</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleToggleSafety() {
  systemArmed = !systemArmed;
  bool closedPolygon = false;
  if (!systemArmed && polygonActive) {
    // Do not silently drop work in progress when the operator hits safety.
    closePolygon("safety engaged", true);
    closedPolygon = true;     // keep closePolygon's message, it is the useful one
  }
  if (systemArmed)         lastAction = "System Armed! Ready.";
  else if (!closedPolygon) lastAction = "Safety Engaged.";
  server.sendHeader("Location", "/");
  server.send(303);
}

// Retired. Capture mode is a property of the target slot now, so this endpoint
// changes nothing - it exists only so an old bookmark or a phone that cached
// the previous dashboard gets a redirect instead of a 404.
void handleSetMode() {
  if (rejectSpeculative()) return;
  lastAction = "Mode follows the target now - pick a button.";
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetTarget() {
  // A browser prefetch must never change what the operator is logging.
  if (rejectSpeculative()) return;
  if (server.hasArg("w")) {
    int w = server.arg("w").toInt();
    int s = w - 1;
    if (s >= 0 && s < NUM_SLOTS) {
      // A LINE in progress refuses to be interrupted.
      //
      // On 14 Nov a mis-tap while driving an effort line ended it, and the
      // only sign was that the next stretch of road went unrecorded until the
      // trigger was pressed again. A line is a deliberate, minutes-long
      // capture with a defined end, so an accidental tap must not be able to
      // finish it - the trigger is the only thing that may. Polygons keep the
      // old behaviour: there the target genuinely has changed, and the ring
      // has to be closed against the target it was started with.
      if (polygonActive && shapeIsLine) {
        lastAction = "LINE RECORDING - press the trigger to end it before changing target";
        Serial.println("Target change refused: a line is being recorded.");
        if (server.hasArg("q")) { server.send(409); return; }
        server.sendHeader("Location", "/");
        server.send(303);
        return;
      }

      // Switching target also switches capture mode, so anything in flight has
      // to be closed against the target it was STARTED with, not the new one.
      if (polygonActive) closePolygon("target changed", true);
      endContinuousRun("target changed");
      currentSlot        = s;
      currentWeed        = slotWeed[s];
      currentControlType = slotControl[s];
      applySlotTuning();
      lastAction = "Target: " + currentWeed + " (" + currentControlType + ") - " + loggingMode;
    }
  }
  // q=1 means the page asked by fetch() and will restyle itself. Redirecting
  // would make it rebuild the whole dashboard, which is the thing that made
  // changing weed feel slow: the state change is trivial, the page is not.
  if (server.hasArg("q")) { server.send(204); return; }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ==========================================================
// MAP VIEW
// ==========================================================
//
// The unit serves DATA; the phone draws. That split is deliberate: rendering a
// map on the ESP32 would mean a much bigger page built on every refresh, and
// page building is what stretches the loop - the thing that nearly cost us the
// trigger. /linedata is a few hundred bytes and costs nothing to produce.
//
// The page works at three levels and falls back on its own:
//   no network at all  -> an auto-scaled SVG of the line's own shape
//   phone has internet -> the same line drawn over Esri imagery
//   layer readable     -> today's already-synced points on top of that
//
// The middle case is the normal field one, because the unit joins the PHONE'S
// hotspot: the phone is then both the access point and the route to the
// internet, so tiles and unit data arrive over the same connection.
void handleLineData() {
  String j = "{\"rec\":";
  j += (polygonActive && shapeIsLine) ? "1" : "0";
  j += ",\"n\":" + String(vertexCount);
  j += ",\"fix\":" + String(gpsUsable() ? "1" : "0");
  if (live_lat != 0.0 || live_lon != 0.0) {
    j += ",\"lat\":" + String(live_lat, 6) + ",\"lon\":" + String(live_lon, 6);
  }
  j += ",\"pts\":" + String(pointsLogged);
  j += ",\"acc\":" + String(live_accuracy_m, 1);
  j += ",\"bat\":" + String(live_battery_pct);
  // Oldest first, so the page can draw it as a trail in capture order.
  j += ",\"t\":\"";
  for (int k = 0; k < trailCount; k++) {
    int idx = (trailCount == TRAIL_MAX) ? ((trailNext + k) % TRAIL_MAX) : k;
    j += String(trailLon[idx], 6) + "," + String(trailLat[idx], 6) +
         "," + String(trailSeg[idx]) + "," + String(trailSlot[idx]) + ";";
  }
  j += "\"";
  // The vertex string is already stored in the form the page wants, so it goes
  // out as-is rather than being rebuilt into JSON arrays.
  j += ",\"c\":\"";
  if (polygonActive && shapeIsLine) j += currentPolygonCoords;
  j += "\"}";
  server.send(200, "application/json", j);
}

void handleMap() {
  // The points layer's query endpoint, derived from the upload URL the unit
  // already holds. NO TOKEN is put in this page: it would end up in the phone's
  // browser cache and history. If the layer is not readable anonymously the
  // overlay simply does not appear, and the line still draws.
  String q = esriPointServiceURL;
  q.replace("/addFeatures", "/query");

  String h = "<!doctype html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>EcoSync map</title>"
             "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
             "<style>html,body{margin:0;background:#1b1b1b;color:#eee;"
             "font-family:system-ui,sans-serif}#m,#f{height:70vh;width:100%;background:#111;display:block}"
             "#f{display:none}#s{padding:10px 14px;font-size:0.95em}"
             "a.b{display:inline-block;background:#2980b9;color:#fff;padding:12px 18px;"
             "margin:8px 14px;border-radius:8px;text-decoration:none;font-weight:bold}</style>"
             "</head><body><div id='m'></div><svg id='f' viewBox='0 0 1000 700'></svg>"
             "<div id='s'>Loading&hellip;</div><div id='lg' style='padding:4px 14px 2px 14px;font-size:0.85em'></div><a class='b' href='/'>&larr; Dashboard</a>"
             "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script><script>";

  h += "var Q=\"" + q + "\";";
  h += "var SLOTS=[";
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (i) h += ",";
    h += "\"" + jsEscape(slotWeed[i]) + "\"";
  }
  h += "];";

  h += R"JS(
var map=null,ln=null,me=null,tr=null,pl=null,got=false,ptsAt=0;
var synced=-1;   // how many of today's points the layer returned, -1 = not asked yet
function pv(c){var o=[],a=c.split(';');for(var i=0;i<a.length;i++){
 var p=a[i].split(',');if(p.length>=2&&p[0]!==''){
 var x=parseFloat(p[0]),y=parseFloat(p[1]);
 if(!isNaN(x)&&!isNaN(y))o.push([y,x]);}}return o;}
// Same string, but keeping the shape number so vertices can be joined up.
function pvs(c){var o=[],a=c.split(';');for(var i=0;i<a.length;i++){
 var p=a[i].split(',');if(p.length>=3&&p[0]!==''){
 var x=parseFloat(p[0]),y=parseFloat(p[1]),g=parseInt(p[2],10),sl=parseInt(p[3],10);
 if(!isNaN(x)&&!isNaN(y))o.push({ll:[y,x],seg:g||0,slot:isNaN(sl)?0:sl});}}return o;}
// Chosen to stay apart from each other AND from the map's own colours: blue is
// a finished shape, green the one recording, white is you.
var SC=['#e74c3c','#f39c12','#9b59b6','#e84393','#2ecc71','#00cec9'];
function sc(i){return SC[i%SC.length];}

// Esri imagery. If Leaflet or the tiles cannot be reached we never get here and
// the SVG takes over instead.
function initMap(){
 if(typeof L==='undefined')return false;
 try{
  map=L.map('m',{zoomControl:true});
  // maxNativeZoom is the fix for tiles vanishing when you zoom in: Esri's
  // imagery runs out at about z18 over rural Australia, and without this
  // Leaflet asks for tiles that do not exist and draws nothing. With it, the
  // last real tile is stretched instead - blurrier, but still a picture.
  L.tileLayer('https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
   {maxZoom:21,maxNativeZoom:18,attribution:'Esri'}).addTo(map);
  map.setView([-34.5,139.5],6);
  return true;
 }catch(e){return false;}
}
var haveMap=initMap();
if(!haveMap){document.getElementById('m').style.display='none';
 document.getElementById('f').style.display='block';}

// Today's already-synced points, queried straight from the layer by the phone.
// Local midnight converted to epoch ms sidesteps the UTC trap entirely: the
// layer stores UTC and this comparison is in the same units.
// Reloaded on a timer, not once. Two officers on the same block each see the
// other's treated points as soon as both have synced, which is the whole value
// of showing the layer rather than just this unit's own session.
function loadPoints(){
 if(!haveMap)return;
 var now=Date.now();if(now-ptsAt<60000)return;ptsAt=now;
 var d=new Date();d.setHours(0,0,0,0);
 var u=Q+'?where=treatment_time%3E%3D'+d.getTime()+'&outFields=weed_species&f=geojson&returnGeometry=true&resultRecordCount=2000';
 fetch(u).then(function(r){return r.json();}).then(function(g){
  if(!g||!g.features||!g.features.length)return;
  if(pl)pl.remove();
  pl=L.geoJSON(g,{pointToLayer:function(f,ll){return L.circleMarker(ll,
   {radius:8,color:'#ffffff',weight:2,fill:false,opacity:0.95});},
   onEachFeature:function(f,l){if(f.properties&&f.properties.weed_species)
    l.bindPopup(f.properties.weed_species);}}).addTo(map);
  synced=g.features.length;
 }).catch(function(){
  // Silence here was wrong: a layer that will not answer looked identical to a
  // layer with no records, and no way to tell them apart on a phone.
  var e=document.getElementById('s');
  e.textContent=e.textContent+'  |  synced points unavailable (layer needs sign-in?)';
 });
}

function drawSvg(v){
 var f=document.getElementById('f');while(f.firstChild)f.removeChild(f.firstChild);
 if(v.length<2)return;
 var la=v.map(function(p){return p[0];}),lo=v.map(function(p){return p[1];});
 var laMin=Math.min.apply(null,la),laMax=Math.max.apply(null,la);
 var loMin=Math.min.apply(null,lo),loMax=Math.max.apply(null,lo);
 var k=Math.cos((laMin+laMax)/2*Math.PI/180);           // east-west is shorter
 var w=(loMax-loMin)*k,hh=(laMax-laMin);
 var sp=Math.max(w,hh)||1e-6,sc=620/sp;
 var pts=v.map(function(p){
  var x=500+((p[1]-(loMin+loMax)/2)*k)*sc;
  var y=350-((p[0]-(laMin+laMax)/2))*sc;                // north is up
  return x.toFixed(1)+','+y.toFixed(1);}).join(' ');
 var ns='http://www.w3.org/2000/svg';
 var pl=document.createElementNS(ns,'polyline');
 pl.setAttribute('points',pts);pl.setAttribute('fill','none');
 pl.setAttribute('stroke','#3498db');pl.setAttribute('stroke-width','4');
 pl.setAttribute('stroke-linejoin','round');f.appendChild(pl);
 var last=pts.split(' ').pop().split(',');
 var c=document.createElementNS(ns,'circle');
 c.setAttribute('cx',last[0]);c.setAttribute('cy',last[1]);c.setAttribute('r','7');
 c.setAttribute('fill','#e74c3c');f.appendChild(c);
 // A scale bar, because an auto-fitted shape has no inherent size.
 var m=sp*111320*0.25,t=document.createElementNS(ns,'text');
 t.setAttribute('x','20');t.setAttribute('y','680');t.setAttribute('fill','#999');
 t.setAttribute('font-size','22');
 t.textContent='about '+(m>1000?(m/1000).toFixed(1)+' km':Math.round(m)+' m')+' across a quarter of this view';
 f.appendChild(t);
}

function tick(){
 fetch('/linedata').then(function(r){return r.json();}).then(function(d){
  var v=pv(d.c||'');
  var s;
  if(d.rec){
   // Live length, worked out the same way the firmware does it on upload, so
   // the figure on the screen matches the one that ends up in the attribute.
   var m=0;
   for(var i=1;i<v.length;i++){
    var k=Math.cos(v[i][0]*Math.PI/180);
    var dy=(v[i][0]-v[i-1][0])*111320,dx=(v[i][1]-v[i-1][1])*111320*k;
    m+=Math.sqrt(dx*dx+dy*dy);
   }
   s='Recording - '+d.n+' vertices, '+(m>1000?(m/1000).toFixed(2)+' km':Math.round(m)+' m');
  }else{
   s=d.pts+' points this session';
  }
  if(!d.fix)s+='  |  NO USABLE FIX';
  else if(d.acc!==undefined)s+='  |  \u00b1'+d.acc+' m';
  if(d.bat!==undefined&&d.bat>0)s+='  |  battery '+d.bat+'%';
  if(synced>=0)s+='  |  '+synced+' synced today';
  document.getElementById('s').textContent=s;
  if(haveMap){
   var ts=pvs(d.t||''),all=[];
   if(ts.length){
    if(tr)tr.remove();
    var g=L.layerGroup(),segs={};
    for(var i=0;i<ts.length;i++){
     all.push(ts[i].ll);
     if(ts[i].seg===0){
      // A standalone point: yellow dot.
      var col=sc(ts[i].slot);
      g.addLayer(L.circleMarker(ts[i].ll,{radius:4,color:'#000',weight:1,
        fillColor:col,fillOpacity:0.95}));
     }else{
      if(!segs[ts[i].seg])segs[ts[i].seg]={pts:[],slot:ts[i].slot};
      segs[ts[i].seg].pts.push(ts[i].ll);
     }
    }
    // Shape vertices joined into the line they came from, in blue so a
    // finished line never looks like a row of points.
    for(var k in segs){if(segs[k].pts.length>1)
     g.addLayer(L.polyline(segs[k].pts,{color:sc(segs[k].slot),weight:4,opacity:0.9}));}
    tr=g.addTo(map);
    if(!got&&all.length){map.fitBounds(L.latLngBounds(all),{padding:[30,30]});got=true;}
   }
   if(v.length>1){
    if(ln)ln.remove();
    ln=L.polyline(v,{color:'#1abc9c',weight:5}).addTo(map);
    if(!got){map.fitBounds(ln.getBounds(),{padding:[30,30]});got=true;}
   }
   if(d.lat&&d.lon){
    if(me)me.remove();
    me=L.circleMarker([d.lat,d.lon],{radius:7,color:'#1b1b1b',weight:3,
      fillColor:'#ffffff',fillOpacity:1}).addTo(map);
    if(!got){map.setView([d.lat,d.lon],17);got=true;}
   }
   loadPoints();
  }else{drawSvg(v.length>1?v:pv(d.t||''));}
 }).catch(function(){});
}
(function(){
 var l=document.getElementById('lg'),h='';
 for(var i=0;i<SLOTS.length;i++){
  h+="<span style='display:inline-block;margin:2px 10px 2px 0;white-space:nowrap'>"
   +"<span style='display:inline-block;width:11px;height:11px;border-radius:50%;"
   +"background:"+sc(i)+";border:1px solid #000;vertical-align:-1px'></span> "
   +SLOTS[i]+"</span>";
 }
 h+="<span style='display:inline-block;margin:2px 10px;white-space:nowrap'>"
  +"<span style='display:inline-block;width:11px;height:11px;border-radius:50%;"
  +"border:2px solid #fff;vertical-align:-1px'></span> synced</span>";
 l.innerHTML=h;
})();
tick();setInterval(tick,3000);
)JS";

  h += "</script></body></html>";
  server.send(200, "text/html", h);
}

// Clears notices that describe something that ALREADY happened and has been
// read. It deliberately does not clear configWarning: that one reports a live
// condition - the unit is running on built-in defaults, with the wrong device
// id and no network - and hiding it would turn a visible fault into a silent
// one.
void handleDismiss() {
  bootNoticeDismissed = true;
  polyRecoveryNote    = "";
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleForceSync() {
  // A browser prefetch must never change what the operator is logging.
  if (rejectSpeculative()) return;
  triggerSync = true;
  lastAction = "Sync Initiated...";
  server.sendHeader("Location", "/");
  server.send(303);
}

// Secrets are NEVER rendered back into the page. type='password' only masks
// them on screen - the value still sits in the HTML source for anyone to read.
// The field is served empty; an empty submission means "keep what is stored".
static String secretField(const char *label, const char *name, bool isSet,
                          const char *hint) {
  String h = "<label>" + String(label) + "</label><br>";
  h += "<input type='password' name='" + String(name) + "' value='' autocomplete='new-password' "
       "placeholder='" + String(isSet ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2 stored - leave blank to keep" : "not set") + "' "
       "style='padding:10px; margin-bottom:4px; width:80%; font-size:1em; text-align:center;'><br>";
  h += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px;'>";
  h += String(hint && hint[0] ? hint : "Leave blank to keep the current value.");
  h += "</div>";
  return h;
}

static String textField(const char *label, const char *name, const String &value,
                        bool isPassword, const char *hint) {
  String h = "<label>" + String(label) + "</label><br>";
  h += "<input type='" + String(isPassword ? "password" : "text") + "' name='" + name +
       "' value='" + htmlEscape(value) +
       "' style='padding:10px; margin-bottom:4px; width:80%; font-size:1em; text-align:center;'><br>";
  if (hint && hint[0]) {
    h += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px;'>" + String(hint) + "</div>";
  } else {
    h += "<div style='margin-bottom:12px;'></div>";
  }
  return h;
}

// Every submitted value is trimmed, because the config reader trims on load.
// Without this a password typed with a trailing space (which mobile keyboards
// add routinely) works until the next reboot and then stops - or vice versa.
static String argTrimmed(const char *name) {
  String v = server.arg(name);
  v.trim();
  return v;
}

// Read-only row, for showing operators a value they must not edit.
static String roRow(const char *label, const String &value) {
  return "<div style='display:flex; justify-content:space-between; width:80%; margin:0 auto 6px auto; font-size:0.8em;'>"
         "<span style='color:#95a5a6;'>" + String(label) + "</span>"
         "<span style='color:white;'>" + htmlEscape(value) + "</span></div>";
}

static String pageHead(const char *title) {
  String h = "<!DOCTYPE html><html><head>";
  h += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  h += "<style>body { background-color: #2c3e50; color: white; text-align: center; font-family: sans-serif; padding-top: 4vh; font-size: 1.05em; } h3 { color:#95a5a6; margin-top:22px; font-size:0.8em; letter-spacing:1px; }</style>";
  h += "</head><body><h2>" + String(title) + "</h2>";
  return h;
}

// ------------------------------------------------------------------
// FIELD SETUP - open to operators. Targets, control methods, job number.
// Deliberately holds nothing sensitive and nothing that affects data quality.
// ------------------------------------------------------------------
void handleConfig() {
  String html = pageHead("Field Setup");
  html += "<form action='/saveConfig' method='POST'>";

  html += "<h3>CAPTURE SLOTS</h3>";
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin:-6px 0 12px 0; padding:0 8%;'>"
          "Each dashboard button carries its own weed, control method, capture mode and timing. "
          "Setting the timing here is what stops you editing it in the field.</div>";

  const char* modeNames[]  = { "POINT", "CONTINUOUS", "POLYGON", "LINE" };
  // Counted from the array, not typed as a literal. The literal 3 that used to
  // be in the loop below outlived the arrival of LINE, so the mode existed in
  // the firmware and in the help text but could not be chosen on the page.
  const int   modeCount    = sizeof(modeNames) / sizeof(modeNames[0]);
  for (int i = 0; i < NUM_SLOTS; i++) {
    String n  = String(i + 1);
    String fT = "t" + n, fC = "c" + n, fM = "m" + n, fA = "a" + n, fB = "b" + n;

    html += "<h4 style='margin:16px 0 4px 0; color:#1abc9c;'>SLOT " + n + "</h4>";
    html += textField("Weed / target", fT.c_str(), slotWeed[i], false, "");
    html += textField("Control method", fC.c_str(), slotControl[i], false, "");

    html += "<label>Capture mode</label><br>";
    html += "<select name='" + fM + "' style='padding:10px; margin-bottom:4px; width:82%; font-size:1em; text-align:center;'>";
    for (int m = 0; m < modeCount; m++) {
      html += "<option value='" + String(modeNames[m]) + "'";
      if (slotMode[i] == modeNames[m]) html += " selected";
      html += ">" + String(modeNames[m]) + "</option>";
    }
    html += "</select>";
    html += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px;'>"
            "POINT = one record per pull. CONTINUOUS = a record every few seconds while held. "
            "POLYGON = walk the perimeter. LINE = press to start, follow the route, "
            "press again to end - bait trails and effort lines.</div>";

    String hintA, hintB;
    if (slotMode[i] == "POINT") {
      hintA = "Cooldown on the same spot (s)";
      hintB = "Movement that logs early (m). 0 = plain timer.";
    } else if (slotMode[i] == "CONTINUOUS") {
      hintA = "Seconds between fixes while the trigger is held";
      hintB = "Release hold (s) - how long the trigger can be up before the run ends. "
              "Releasing and pulling again on the same weed keeps one run.";
    } else if (slotMode[i] == "LINE") {
      hintA = "Distance travelled before a new vertex (m)";
      hintB = "Idle timeout (s). 0 = never, which is what you want - a line is "
              "ended by pressing the trigger again, not by a timer.";
    } else {
      hintA = "Distance travelled before a new vertex (m)";
      hintB = "Trigger silence that closes the polygon (s)";
    }
    html += textField("Timing A", fA.c_str(), String(slotA[i], 1), false, hintA.c_str());
    html += textField("Timing B", fB.c_str(), String(slotB[i], 1), false, hintB.c_str());
  }
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin:0 0 12px 0; padding:0 8%;'>"
          "Save, then reopen this page to see the timing labels update for a mode you just changed.</div>";

  html += "<h3>JOB</h3>";
  html += textField("Job Number (ID)", "job", currentJobNumber, false, "");
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin:-6px 0 12px 0; padding:0 8%;'>Control method is written to control_type in ArcGIS. If that field uses a coded-value domain, enter the CODE, not the description.</div>";

  html += "<br><input type='submit' value='SAVE TO SD CARD' style='background-color:#27ae60; color:white; padding:15px 30px; font-size:1em; border:none; border-radius:8px;'>";
  html += "</form>";

  // Read-only. An operator hitting rejections can see the thresholds and read
  // them back over the radio, but cannot quietly widen them.
  html += "<h3>CURRENT CAPTURE SETTINGS (READ ONLY)</h3>";
  html += roRow("Max GPS accuracy", String(gpsMaxAccM, 1) + " m");
  html += roRow("Point cooldown", String(pointCooldownS, 1) + " s");
  html += roRow("Movement that logs early", pointMinDistM > 0.0 ? String(pointMinDistM, 1) + " m" : String("off"));
  html += roRow("Polygon / line idle timeout", polyTimeoutS > 0.0 ? (String(polyTimeoutS, 1) + " s") : String("off"));
  html += roRow("Vertex spacing", String(polyMinVertexM, 1) + " m");
  html += roRow("Smoothing tolerance", polySimplifyM > 0.0 ? String(polySimplifyM, 1) + " m" : String("off"));
  html += "<div style='font-size:0.7em; color:#7f8c8d; margin:10px 8% 0 8%;'>These control data quality and are set by your administrator. Widening them silently degrades every record.</div>";

  html += "<br><br><a href='/wifiscan' style='color:#3498db; text-decoration:none; font-size:0.9em;'>\xF0\x9F\x93\xB6 Wi-Fi Diagnostics</a>";
  html += "<br><br><a href='/advanced' style='color:#e67e22; text-decoration:none; font-size:0.9em;'>\xF0\x9F\x94\x92 Advanced Settings</a>";
  html += "<br><br><a href='/' style='color:#bdc3c7; text-decoration:none;'>Return to Dashboard</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Accepts ONLY the field-setup arguments. This endpoint is unauthenticated, so
// it must never touch connectivity or capture tuning - otherwise a hand-crafted
// POST here would bypass the password on /advanced entirely.
void handleSaveConfig() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    String n  = String(i + 1);
    String fT = "t" + n, fC = "c" + n, fM = "m" + n, fA = "a" + n, fB = "b" + n;

    if (server.hasArg(fT.c_str())) slotWeed[i]    = argTrimmed(fT.c_str());
    if (server.hasArg(fC.c_str())) slotControl[i] = argTrimmed(fC.c_str());

    String oldMode = slotMode[i];
    if (server.hasArg(fM.c_str())) {
      String m = argTrimmed(fM.c_str()); m.toUpperCase();
      if (m == "POINT" || m == "CONTINUOUS" || m == "POLYGON" ||
          m == "LINE") slotMode[i] = m;
    }

    if (slotMode[i] != oldMode) {
      // The two numbers mean different things in different modes, so carrying
      // a cooldown of 10 over into "seconds between fixes" would be nonsense.
      // Blank them and let sanitiseConfig seed the new mode's defaults.
      slotA[i] = -1.0;
      slotB[i] = -1.0;
    } else {
      if (server.hasArg(fA.c_str())) slotA[i] = argTrimmed(fA.c_str()).toFloat();
      if (server.hasArg(fB.c_str())) slotB[i] = argTrimmed(fB.c_str()).toFloat();
    }
  }
  if (server.hasArg("job")) currentJobNumber = argTrimmed("job");

  sanitiseConfig();

  // A target rename mid-capture would otherwise mislabel the saved feature.
  if (polygonActive) closePolygon("config changed", true);
  endContinuousRun("config changed");

  // Re-derive from the slot that is ACTUALLY selected. This used to snap back
  // to slot 1 on every save, which is why the active weed kept reverting.
  currentWeed        = slotWeed[currentSlot];
  currentControlType = slotControl[currentSlot];
  applySlotTuning();

  if (saveConfigToSD()) lastAction = "Field setup saved.";
  else                  lastAction = "CONFIG SAVE FAILED - check SD card";

  server.sendHeader("Location", "/");
  server.send(303);
}

// ------------------------------------------------------------------
// WI-FI DIAGNOSTICS - deliberately unauthenticated, because the whole point
// is to be reachable when the network is broken. It reveals only SSIDs, which
// are broadcast to anyone with a phone anyway.
// ------------------------------------------------------------------
void handleWifiScan() {
  String html = pageHead("Wi-Fi Diagnostics");

  html += "<div style='width:86%; margin:0 auto 18px auto; text-align:left; font-size:0.85em;'>";
  html += "<b>Configured network:</b> " + htmlEscape(wifiSsid.length() ? wifiSsid : String("(none set)")) + "<br>";
  html += "<b>Trigger presses accepted:</b> " + String(triggerAccepted) +
          " &nbsp; of " + String(triggerPresses) + " edges"
          " &nbsp; (pin changes " + String(triggerRawChanges) + ")<br>";
  html += "<b>Hold to log:</b> " + String(triggerHoldMs) + " ms"
          " &nbsp; (" + String(triggerTooBrief) + " touches too brief, "
          + String(triggerRefused) + " inside the lockout)<br>";
  html += "<div style='font-size:0.85em; opacity:0.75; margin-bottom:6px;'>"
          "Accepted is the figure to read: one per real press. Edges counts every "
          "interrupt, bounce included, so it sits well above it. "
          "<b>Too brief</b> counts pads touched but not held long enough to log - "
          "a few of those over a day is the setting doing its job. If it climbs "
          "while points go missing, lower hold to log in config.txt; if stray "
          "points are still appearing, raise it.</div>";
  html += "<b>Starlink password stored:</b> " + String(wifiPass.length() ? "yes" : "NO") + "<br>";
  html += "<b>Hotspot SSID:</b> " + htmlEscape(wifiSsid2.length() ? wifiSsid2 : String("(none)")) + "<br>";
  html += "<b>Hotspot password stored:</b> " + String(wifiPass2.length() ? "yes" : "NO") + "<br>";
  html += "<b>Station status:</b> " + String(wifiStatusName(WiFi.status())) + "<br>";
  if (wifiLastError.length()) html += "<b>Last failure:</b> " + htmlEscape(wifiLastError) + "<br>";
  html += "<b>Access point:</b> " + String(apMode ? "running" : "off") + "<br>";
  html += "<b>Transmit power:</b> " + String(wifiTxDbm, 1) + " dBm";
  if (wifiTxDbm < 15.0) html += " <span style='color:#f39c12;'>(reduced)</span>";
  html += "</div>";

  html += "<p style='font-size:0.8em; color:#bdc3c7;'>Scanning 2.4 GHz&hellip;</p>";

  int n = WiFi.scanNetworks();
  bool foundConfigured = false;

  if (n <= 0) {
    html += "<p style='color:#e74c3c;'>No networks found at all. The aerial may be "
            "disconnected, or nothing is broadcasting on 2.4 GHz within range.</p>";
  } else {
    html += "<table style='width:90%; margin:0 auto; font-size:0.8em; border-collapse:collapse;'>";
    html += "<tr style='color:#95a5a6;'><th align='left'>Network</th><th>Signal</th><th>Ch</th><th>Security</th></tr>";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      bool isMine = (wifiSsid.length()  > 0 && ssid == wifiSsid) ||
                    (wifiSsid2.length() > 0 && ssid == wifiSsid2);
      if (isMine) foundConfigured = true;

      String colour = "#2ecc71";
      if (rssi < -80)      colour = "#e74c3c";
      else if (rssi < -70) colour = "#f39c12";

      html += "<tr style='border-top:1px solid #34495e;'>";
      html += "<td align='left'>" + String(isMine ? "&#9654; " : "") + htmlEscape(ssid) + "</td>";
      html += "<td align='center' style='color:" + colour + ";'>" + String(rssi) + " dBm</td>";
      html += "<td align='center'>" + String(WiFi.channel(i)) + "</td>";
      html += "<td align='center'>" + String(WiFi.encryptionType(i) == 0 ? "open" : "secured") + "</td>";
      html += "</tr>";
    }
    html += "</table>";
  }
  WiFi.scanDelete();

  html += "<div style='width:86%; margin:18px auto; text-align:left; font-size:0.85em; "
          "background:#34495e; padding:12px; border-radius:6px;'>";
  if (!wifiSlotConfigured(0) && !wifiSlotConfigured(1)) {
    html += "No network is configured. Set the Starlink network or your phone "
            "hotspot under Advanced Settings.";
  } else if (foundConfigured) {
    html += "<b style='color:#2ecc71;'>Your network was found.</b> If it still will not join, "
            "the password is the most likely cause &mdash; re-enter it under Advanced Settings. "
            "If the signal above is worse than about &minus;75 dBm, try raising the transmit power.";
  } else {
    html += "<b style='color:#e74c3c;'>Your network was NOT found.</b> This unit is 2.4 GHz only. "
            "Check the name matches exactly including capitals, and that the router is not set "
            "to 5 GHz only for that network. On Starlink, enable the 2.4 GHz band or a separate "
            "2.4 GHz network name.";
  }
  html += "</div>";

  // Receiver diagnostics, ALWAYS shown. These used to live inside the NMEA feed
  // panel, which meant turning the phone feed off to save battery also hid the
  // answer to "is the GPS working". Feed state and receiver state are different
  // questions and only one of them is optional.
  html += "<div style='width:86%; margin:0 auto 18px auto; text-align:left; font-size:0.85em; "
          "background:#2c3e50; padding:12px; border-radius:6px;'>";
  html += "<b>GNSS receiver</b><br>";
  html += "Transport: " + htmlEscape(gnssTransport);
  html += " &nbsp;|&nbsp; PROTVER: ";
  html += (gnssProtVer.length() ? htmlEscape(gnssProtVer) : String("unknown"));
  html += "<br>Fix type: " + String(live_fix_type) +
          " &nbsp;|&nbsp; Satellites: " + String(live_siv) +
          " &nbsp;|&nbsp; Accuracy: " +
          (live_accuracy_m > 0.0 ? String(live_accuracy_m, 2) + " m" : String("--"));

  // Restart history. Without this a reboot is invisible: the unit simply
  // appears with the safety on and no explanation.
  html += "<br><b>Uptime: </b>" + uptimeText();
  html += " &nbsp;|&nbsp; <b>Restarts: </b>" + String(bootCount);
  html += "<br><b>Last restart: </b>";
  html += (bootWasUnexpected ? "<span style='color:#e74c3c;'>" : "<span style='color:#2ecc71;'>");
  html += htmlEscape(bootResetReason) + "</span>";
  if (polyRecoveryNote.length()) {
    html += "<br><b>Polygon journal: </b><span style='color:#f39c12;'>"
            + htmlEscape(polyRecoveryNote) + "</span>";
  }
  if (speculativeBlocked > 0) {
    html += "<br><b>Browser prefetches refused: </b><span style='color:#f39c12;'>"
            + String(speculativeBlocked) + "</span>";
  }

  // The line this whole panel exists for.
  html += "<br><b>SBAS corrections: </b>";
  if (live_diff_soln) {
    html += "<span style='color:#2ecc71;'>APPLIED NOW</span>";
  } else if (diffSolnEverSeen) {
    html += "<span style='color:#f39c12;'>not right now - but seen "
            + String(diffSolnFirstMs / 1000) + "s after boot</span>";
  } else {
    html += "<span style='color:#e74c3c;'>never seen since boot</span>";
  }
  html += "<div style='font-size:0.85em; color:#bdc3c7; margin-top:6px;'>";
  if (live_diff_soln) {
    html += "The module is using SBAS. This is the 1.0 m CEP regime rather than 1.5 m.";
  } else if (diffSolnEverSeen) {
    html += "Corrections have worked, so the module and configuration are fine - "
            "this is sky view. Expect it to drop under canopy and return in the open.";
  } else {
    html += "Give it 10-15 minutes outside with clear sky: the SBAS satellite is "
            "geostationary and the correction data takes several minutes to download. "
            "If it never appears, SBAS may need enabling on the module in u-center.";
  }
  html += "</div>";

  html += "<br>SouthPAN config: ";
  html += (sbasConfigOk ? "<span style='color:#2ecc71;'>" : "<span style='color:#e74c3c;'>");
  html += htmlEscape(sbasConfigDetail) + "</span>";
  html += "<br>GST enable request: ";
  if (!gstEnableAttempted)   html += "not attempted";
  else if (gstEnableOk)      html += "accepted by module";
  else                       html += "<span style='color:#e74c3c;'>REFUSED by module</span>";
  html += "</div>";

  if (nmeaOut) {
    html += "<div style='width:86%; margin:0 auto 18px auto; text-align:left; font-size:0.85em; "
            "background:#34495e; padding:12px; border-radius:6px;'>";
    html += "<b>GNSS feed to phone apps</b><br>";
    html += "Address: " + htmlEscape(nmeaFeedAddresses()) + "<br>";
    html += "Phone connected: " + String((nmeaClient && nmeaClient.connected()) ? "yes" : "no") + "<br>";
    html += "Data sent: " + String(nmeaBytesSent / 1024) + " KB";
    html += " &nbsp; Dropped: " + String(nmeaDropped);
    html += " &nbsp; Stalls: " + String(nmeaStalls) + "<br>";
    html += "Sentences seen &mdash; GGA: " + String(nmeaSeenGGA ? "yes" : "NO") +
            " &nbsp; RMC: " + String(nmeaSeenRMC ? "yes" : "NO") +
            " &nbsp; GST: " + String(nmeaSeenGST ? "yes" : "NO") + "<br>";

    // The full list is the diagnostic that matters: it separates "this module
    // does not send GST" from "no NMEA is reaching us at all".
    html += "All types seen: ";
    html += (nmeaTypesSeen.length() ? htmlEscape(nmeaTypesSeen) : String("(none yet)"));

    if (nmeaLastSentence.length()) {
      html += "<div style='margin-top:8px; font-family:monospace; font-size:0.85em; color:#95a5a6; word-break:break-all;'>"
              + htmlEscape(nmeaLastSentence) + "</div>";
    }
    html += "</div>";

    if (!nmeaSeenGST) {
      html += "<div style='width:86%; margin:0 auto 18px auto; text-align:left; font-size:0.85em; "
              "background:#7d6608; padding:12px; border-radius:6px;'>";
      html += "<b>GST is missing.</b> Apps will show position but no accuracy figure.<br><br>";
      if (nmeaTypesSeen.length() == 0) {
        html += "No NMEA at all is reaching the relay, so this is not a GST problem. Check "
                "that the GPS was detected at boot and that the feed is enabled.";
      } else if (gstEnableOk) {
        html += "The module accepted the request but is not sending GST. That normally means "
                "this module generation does not produce it. The list above is what it does send.";
      } else {
        html += "The module refused the request. Retry below - the attempt made at boot can be "
                "lost while the I2C port is still being reconfigured.";
      }
      html += "<br><br><a href='/gstretry' style='background:#e67e22; color:white; padding:8px 16px; "
              "text-decoration:none; border-radius:5px; display:inline-block;'>Retry GST enable now</a>";
      html += "</div>";
    }
  }

  html += "<a href='/wifiscan' style='color:#e67e22; text-decoration:none;'>Scan again</a>";
  html += "<br><br><a href='/config' style='color:#bdc3c7; text-decoration:none;'>Back to Field Setup</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Re-send the GST request without a reflash, then persist it so it survives the
// next power cycle. Diagnostics-level, the same as /wifiscan: it turns on a
// status sentence and cannot affect logged data or expose a credential.
void handleGstRetry() {
  // A browser prefetch must never change what the operator is logging.
  if (rejectSpeculative()) return;
  bool ok = enableGst();
  if (ok) myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_MSGCONF);

  // Clear the observation so the page reports what happens from here, rather
  // than what was true before the retry.
  nmeaSeenGST = false;
  lastAction  = ok ? "GST enable accepted - watch the diagnostics page"
                   : "GST enable refused by the GNSS module";

  server.sendHeader("Location", "/wifiscan");
  server.send(302, "text/plain", "");
}

// ------------------------------------------------------------------
// ADVANCED - behind the admin password. Connectivity and capture tuning.
// ------------------------------------------------------------------
void handleAdvanced() {
  if (!requireAuth()) return;

  String html = pageHead("Advanced Settings");
  if (adminPass.length() == 0) {
    html += "<div style='background:#c0392b; color:white; padding:12px; margin:0 5% 16px 5%; border-radius:6px; font-size:0.8em;'>"
            "<b>This page has no password.</b> Anyone on this network can open it and change "
            "your API key, Wi-Fi and capture settings. Set a Device Login password below.</div>";
  }
  html += "<form action='/saveAdvanced' method='POST'>";

  // Shown, not editable. This is the unit's identity and it ends up on every
  // record it uploads; a typo made on a phone in the field would quietly
  // mislabel a day's work. Changing it is a deliberate act on the SD card.
  html += "<h3>THIS UNIT</h3>";
  html += roRow("Device ID", currentJobID);
  html += roRow("Hostname", mdnsHostname() + ".local");
  html += "<div style='font-size:0.75em; color:#95a5a6; margin:0 auto 14px auto; width:80%;'>"
          "Written to job_id on every record. Set device_id in config.txt on the SD card.</div>";

  html += "<h3>WI-FI</h3>";
  html += textField("1. Starlink / vehicle network (SSID)", "wssid", wifiSsid, false, "Leave both blank to stay in setup access-point mode");
  html += secretField("Starlink password", "wpass", wifiPass.length() > 0, "");
  html += textField("2. Phone hotspot (SSID)", "wssid2", wifiSsid2, false, "Preferred in the field - the phone keeps its data, so Field Maps still works");
  html += secretField("Hotspot password", "wpass2", wifiPass2.length() > 0, "iPhone: turn on Maximise Compatibility. This unit is 2.4 GHz only.");

  html += textField("Transmit power (dBm)", "txdbm", String(wifiTxDbm, 1), false, "2.0 to 19.5. Raise if the unit cannot reach the router from the vehicle.");

  html += "<h3>GNSS FEED TO PHONE APPS</h3>";
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px; padding:0 8%;'>Lets ArcGIS QuickCapture or Survey123 log at this unit's accuracy instead of the handset's. In the app, add a receiver using a <b>Network connection</b> and enter the address shown on the dashboard.</div>";
  html += "<div style='font-size:0.8em; margin-bottom:10px;'><label><input type='checkbox' name='nmeaon' value='1'";
  html += nmeaOut ? " checked" : "";
  html += "> Serve GNSS position to phone apps</label></div>";
  html += textField("Feed port", "nmeaport", String(nmeaPort), false, "Default 10110");

  html += "<h3>SETUP ACCESS POINT</h3>";
  html += "<div style='font-size:0.8em; margin-bottom:10px;'><label><input type='checkbox' name='apalways' value='1'";
  html += apAlways ? " checked" : "";
  html += "> Keep the setup access point running at all times</label>"
          "<div style='font-size:0.85em; color:#bdc3c7; padding:0 8%;'>Costs about 45 mA, roughly 2.25 %/h on a 2000 mAh cell - it is the single biggest thing you can switch off. Leave it unticked and use the idle shutdown and window below.</div></div>";
  html += textField("AP Name", "apssid", apSsid, false, "");
  html += secretField("AP Password", "appass", apPass.length() > 0, "Minimum 8 characters. Blank keeps the current one.");
  html += textField("AP idle shutdown (min)", "apidle", String(apIdleStopMin, 1), false,
                    "Minutes with no phone connected before the access point switches itself off, to save about 2.25 %/h. 0 = never.");
  html += textField("AP window every (min)", "apwinmin", String(apWindowMin, 1), false,
                    "After it shuts down, the access point reopens on this cycle so you can always get back in - including when the unit is on Wi-Fi and your phone is on mobile data. 0 = never reopen.");
  html += textField("AP window length (s)", "apwinsec", String(apWindowSec, 0), false,
                    "How long each window stays open. 30 to 600. Connect during one and it stays up. 15 min / 120 s costs about 0.3 %/h.");

  html += "<h3>ARCGIS</h3>";
  html += secretField("API Key / Token", "token", esriToken.length() > 0,
                      "Leave blank to keep the current key. To remove it entirely, clear esri_token in config.txt on the card.");

  html += "<h3>DEVICE LOGIN</h3>";
  html += textField("Username", "auser", adminUser, false, "");
  html += secretField("Password", "apassw", adminPass.length() > 0,
                      "Leave blank to keep the current password. Locked out? Hold the spray trigger while switching the unit on to clear it.");

  html += "<h3>CAPTURE TUNING</h3>";
  html += "<div style='font-size:0.7em; color:#e67e22; margin-bottom:12px; padding:0 8%;'>These decide what gets recorded and how accurately. Widening them will not produce more data, only worse data.</div>";
  html += textField("Max GPS accuracy (m)", "acc", String(gpsMaxAccM, 1), false, "Sprays are rejected above this figure");
  html += textField("Min vertex spacing (m)", "pdist", String(polyMinVertexM, 1), false, "Distance travelled before a new polygon point");
  html += textField("Smoothing tolerance (m)", "psimp", String(polySimplifyM, 1), false, "Removes GPS jitter from polygon edges on upload. 0 disables.");
  html += textField("Polygon / line idle timeout (s)", "ptime", String(polyTimeoutS, 1), false, "Trigger silence that closes an open shape. 0 = never, which is what LINE wants");

  html += "<h3>TRIGGER</h3>";
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px; padding:0 8%;'>How long the pad must be held shut before a press is logged. Raise it if a finger resting on the pad logs points you did not mean; lower it if deliberate presses are being missed. The serial log reports every touch that was too brief.</div>";
  html += textField("Hold to log (ms)", "thold", String(triggerHoldMs), false, "20-2000 ms. 150 suits the Olight pad. 0 = log on the edge, no duration check.");

  html += "<h3>POINT DE-DUPLICATION</h3>";
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px; padding:0 8%;'>A large weed can take many trigger pulls. A press logs a new point when EITHER the cooldown has passed OR you have moved far enough to be at a different plant.</div>";
  html += textField("Cooldown on the same spot (s)", "pcool", String(pointCooldownS, 1), false, "Seconds before the same spot logs again");
  html += textField("Movement that logs early (m)", "pmind", String(pointMinDistM, 1), false, "Distance that releases the cooldown. 0 = plain timer.");

  html += "<br><input type='submit' value='SAVE TO SD CARD' style='background-color:#27ae60; color:white; padding:15px 30px; font-size:1em; border:none; border-radius:8px;'>";
  html += "</form>";
  html += "<br><br><a href='/config' style='color:#bdc3c7; text-decoration:none;'>Back to Field Setup</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// Accepts ONLY the advanced arguments, and only with a valid login.
void handleSaveAdvanced() {
  if (!requireAuth()) return;

  String oldSsid = wifiSsid, oldPass = wifiPass;
  String oldSsid2 = wifiSsid2, oldPass2 = wifiPass2;

  if (server.hasArg("wssid"))  wifiSsid       = argTrimmed("wssid");
  if (server.hasArg("txdbm"))  wifiTxDbm      = argTrimmed("txdbm").toFloat();
  apAlways = server.hasArg("apalways");   // unchecked boxes are simply absent
  bool wasOn = nmeaOut;
  int  oldPort = nmeaPort;
  nmeaOut = server.hasArg("nmeaon");
  if (server.hasArg("nmeaport")) nmeaPort = argTrimmed("nmeaport").toInt();
  sanitiseConfig();
  if (nmeaOut != wasOn || nmeaPort != oldPort) {
    nmeaClient.stop();
    nmeaServerStarted = false;   // maintainNmeaServer will rebind on the new port
  }
  // Empty submission = keep stored value, because the field is served empty.
  if (server.hasArg("wpass")  && server.arg("wpass").length()  > 0) wifiPass  = argTrimmed("wpass");
  if (server.hasArg("wssid2")) wifiSsid2 = argTrimmed("wssid2");
  if (server.hasArg("wpass2") && server.arg("wpass2").length() > 0) wifiPass2 = argTrimmed("wpass2");
  if (server.hasArg("apssid")) apSsid         = argTrimmed("apssid");
  if (server.hasArg("appass") && argTrimmed("appass").length() > 0) apPass    = argTrimmed("appass");
  if (server.hasArg("apidle")) apIdleStopMin  = argTrimmed("apidle").toFloat();
  if (server.hasArg("apwinmin")) apWindowMin  = argTrimmed("apwinmin").toFloat();
  if (server.hasArg("apwinsec")) apWindowSec  = argTrimmed("apwinsec").toFloat();
  if (server.hasArg("token")  && server.arg("token").length()  > 0) esriToken = argTrimmed("token");
  if (server.hasArg("auser"))  adminUser      = argTrimmed("auser");
  if (server.hasArg("apassw") && argTrimmed("apassw").length() > 0) adminPass = argTrimmed("apassw");
  if (server.hasArg("acc"))    gpsMaxAccM     = argTrimmed("acc").toFloat();
  if (server.hasArg("pdist"))  polyMinVertexM = argTrimmed("pdist").toFloat();
  if (server.hasArg("psimp"))  polySimplifyM  = argTrimmed("psimp").toFloat();
  if (server.hasArg("ptime"))  polyTimeoutS   = argTrimmed("ptime").toFloat();
  if (server.hasArg("thold"))  triggerHoldMs  = (unsigned long)argTrimmed("thold").toInt();
  if (server.hasArg("pcool"))  pointCooldownS = argTrimmed("pcool").toFloat();
  if (server.hasArg("pmind"))  pointMinDistM  = argTrimmed("pmind").toFloat();

  sanitiseConfig();

  // Ticking "keep the AP up" while the AP happens to be down did nothing:
  // nothing in maintainWiFi() starts one while the station side is connected.
  // Easy to miss before, because the AP was almost always up already.
  if (apAlways && !apMode) startAccessPoint();

  if (saveConfigToSD()) lastAction = "Advanced settings saved.";
  else                  lastAction = "CONFIG SAVE FAILED - check SD card";

  if (wifiSsid != oldSsid || wifiPass != oldPass ||
      wifiSsid2 != oldSsid2 || wifiPass2 != oldPass2) {
    wifiReconfigured = true;
    wifiSlot = 0;          // start again from the top of the list
  }
  lastWifiAttempt = 0;   // let maintainWiFi retry straight away

  server.sendHeader("Location", "/");
  server.send(303);
}
// ==========================================
// 11. ECOSYNC TO ESRI SYNC
// ==========================================
//
//  Rewritten for production. Key properties:
//    * Features are POSTed in batches with rollbackOnFailure=true, so a batch
//      is applied by ArcGIS all-or-nothing.
//    * On any failure the pending file is REWRITTEN with only the records that
//      have not been accepted yet, so a retry can never re-upload a record that
//      already landed.
//    * All string attributes are JSON-escaped and the whole POST body is
//      URL-encoded, so target/job names containing spaces, & + " \ etc. are safe.
//    * Records with unusable geometry are moved to /rejected.csv rather than
//      being silently dropped or pushed to ArcGIS as 0,0.
//    * Points and polygons are independent: a point failure no longer aborts
//      the polygon sync.
//

// Batch limits. Keep MAX_BATCH_BYTES conservative -- the encoded body is
// roughly 2-3x this size and has to fit in heap alongside the HTTP buffers.
const int MAX_BATCH_FEATURES = 25;
const int MAX_BATCH_BYTES    = 4000;

// ------------------------------------------------------------------
// 11a. Encoding helpers
// ------------------------------------------------------------------
String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)(uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

String urlEncode(const String &in) {
  // NB: do not name this HEX - Arduino's Print.h defines HEX as a macro.
  static const char *HEX_DIGITS = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3 + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in.charAt(i);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 0x0F];
    }
  }
  return out;
}

// ------------------------------------------------------------------
// 11b. Quarantine for unusable records
// ------------------------------------------------------------------
void quarantineLine(const char *reason, const String &line) {
  Serial.println(String("Rejected (") + reason + "): " + line);
  File f = SD.open("/rejected.csv", FILE_APPEND);
  if (f) {
    f.println(String(reason) + "|" + line);
    f.close();
  }
}

// ------------------------------------------------------------------
// 11c. Response validation
// ------------------------------------------------------------------
// ArcGIS returns {"addResults":[{"objectId":n,"success":true}, ...]}.
// The batch is only accepted if every feature we sent came back successful.
bool esriBatchOk(int httpCode, const String &resp, int expected) {
  if (httpCode != 200) return false;
  if (resp.indexOf("\"error\"") >= 0) return false;

  int successes = 0;
  int idx = 0;
  while ((idx = resp.indexOf("\"success\"", idx)) >= 0) {
    int colon = resp.indexOf(':', idx);
    if (colon < 0) break;
    int v = colon + 1;
    while (v < (int)resp.length() && resp.charAt(v) == ' ') v++;
    if (resp.substring(v, v + 4) == "true") successes++;
    idx = v + 1;
  }
  return (expected > 0 && successes == expected);
}

// ------------------------------------------------------------------
// 11c2. Polygon smoothing (Douglas-Peucker)
// ------------------------------------------------------------------
// GPS noise of +/-1-2m is comparable to the vertex spacing, so a driven
// outline zigzags. This drops vertices that sit within epsMetres of the line
// between their neighbours, which removes the jitter while preserving every
// real corner. Iterative (explicit stack) because recursion on a 1000-vertex
// ring could exhaust the ESP32 loop-task stack.
int simplifyPolyline(double *xs, double *ys, int n, double epsMetres) {
  if (epsMetres <= 0.0 || n <= 2) return n;

  static bool     keep[MAX_POLY_VERTICES];
  static uint16_t stkFirst[MAX_POLY_VERTICES];
  static uint16_t stkLast[MAX_POLY_VERTICES];

  for (int i = 0; i < n; i++) keep[i] = false;
  keep[0] = true;
  keep[n - 1] = true;

  // Work in local metres so the tolerance is a real distance.
  const double ky = 111320.0;
  const double kx = 111320.0 * cos(ys[0] * 0.017453292519943295);

  int sp = 0;
  stkFirst[sp] = 0; stkLast[sp] = (uint16_t)(n - 1); sp++;

  while (sp > 0) {
    sp--;
    int first = stkFirst[sp];
    int last  = stkLast[sp];
    if (last <= first + 1) continue;

    double x1 = xs[first] * kx, y1 = ys[first] * ky;
    double x2 = xs[last]  * kx, y2 = ys[last]  * ky;
    double dx = x2 - x1, dy = y2 - y1;
    double segLen = sqrt(dx * dx + dy * dy);

    double worst = -1.0;
    int    worstIdx = -1;
    for (int i = first + 1; i < last; i++) {
      double px = xs[i] * kx, py = ys[i] * ky;
      double d;
      if (segLen < 1e-9) {
        // Endpoints coincide (a closed loop): fall back to radial distance.
        double ex = px - x1, ey = py - y1;
        d = sqrt(ex * ex + ey * ey);
      } else {
        d = fabs(dy * px - dx * py + x2 * y1 - y2 * x1) / segLen;
      }
      if (d > worst) { worst = d; worstIdx = i; }
    }

    if (worst > epsMetres && worstIdx > 0) {
      keep[worstIdx] = true;
      if (sp + 2 <= MAX_POLY_VERTICES - 1) {
        stkFirst[sp] = (uint16_t)first;    stkLast[sp] = (uint16_t)worstIdx; sp++;
        stkFirst[sp] = (uint16_t)worstIdx; stkLast[sp] = (uint16_t)last;     sp++;
      }
    }
  }

  // Count survivors BEFORE touching the arrays.
  //
  // The previous version compacted first and then returned the original n when
  // fewer than 3 vertices survived - "never simplify a polygon out of
  // existence". But the compaction had already run: xs[0] held the first
  // vertex and xs[1] had been OVERWRITTEN with the last, while xs[2..n-1] were
  // still the untouched originals. The caller then built a ring from n
  // vertices whose second point was the final one - scrambled, usually
  // self-intersecting, and rejected outright by ArcGIS.
  //
  // It only fired when fewer than 3 vertices survived, which is exactly the
  // case of a small patch: nothing on a 1 m ring deviates more than a 2 m
  // tolerance from the chord, so every one of them hit this path.
  int kept = 0;
  for (int i = 0; i < n; i++) if (keep[i]) kept++;
  if (kept < 3) return n;              // arrays untouched, caller gets the original

  int m = 0;
  for (int i = 0; i < n; i++) {
    if (keep[i]) { xs[m] = xs[i]; ys[m] = ys[i]; m++; }
  }
  return m;
}

// ------------------------------------------------------------------
// 11d. Feature builders. Return "" if the record is unusable.
// ------------------------------------------------------------------
String buildPointFeature(const String &line) {
  int c1 = line.indexOf(',');            if (c1 < 0) return "";
  int c2 = line.indexOf(',', c1 + 1);    if (c2 < 0) return "";
  int c3 = line.indexOf(',', c2 + 1);    if (c3 < 0) return "";
  int c4 = line.indexOf(',', c3 + 1);    if (c4 < 0) return "";

  String weed      = line.substring(0, c1);
  double lat       = line.substring(c1 + 1, c2).toDouble();
  double lon       = line.substring(c2 + 1, c3).toDouble();
  String epochStr  = line.substring(c3 + 1, c4);

  // control_type (6th) and gps_accuracy_m (7th) were both added later. Records
  // from earlier firmware simply have fewer fields and still upload, just
  // without those attributes. Never require a field a stored record may lack.
  int c5 = line.indexOf(',', c4 + 1);
  String jobNumber, controlType, accuracyStr, runIdStr, captureMode;
  if (c5 < 0) {
    jobNumber = line.substring(c4 + 1);
  } else {
    jobNumber = line.substring(c4 + 1, c5);
    int c6 = line.indexOf(',', c5 + 1);
    if (c6 < 0) { controlType = line.substring(c5 + 1); }
    else {
      controlType = line.substring(c5 + 1, c6);
      // run_id (8th) and capture_mode (9th) are newer again. Same rule: a
      // record written before they existed still uploads, just without them.
      int c7 = line.indexOf(',', c6 + 1);
      if (c7 < 0) { accuracyStr = line.substring(c6 + 1); }
      else {
        accuracyStr = line.substring(c6 + 1, c7);
        int c8 = line.indexOf(',', c7 + 1);
        if (c8 < 0) { runIdStr = line.substring(c7 + 1); }
        else        { runIdStr    = line.substring(c7 + 1, c8);
                      captureMode = line.substring(c8 + 1); }
      }
    }
  }
  captureMode.trim();

  // Reject null island and out-of-range coordinates.
  if (lat == 0.0 && lon == 0.0) return "";
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return "";

  String esriTimeMs = epochStr + "000";

  String attrs = "\"weed_species\":\"" + jsonEscape(weed) + "\"," +
                 "\"treatment_time\":" + esriTimeMs + "," +
                 "\"job_id\":\"" + jsonEscape(String(currentJobID)) + "\"," +
                 "\"Job_Number\":\"" + jsonEscape(jobNumber) + "\"";
  if (controlType.length() > 0) {
    attrs += ",\"control_type\":\"" + jsonEscape(controlType) + "\"";
  }
  if (accuracyStr.length() > 0) {
    double a = accuracyStr.toDouble();
    if (a > 0.0) attrs += ",\"gps_accuracy_m\":" + String(a, 2);
  }
  if (runIdStr.length() > 0 && runIdStr != "0") {
    attrs += ",\"run_id\":\"" + jsonEscape(runIdStr) + "\"";
  }
  if (captureMode.length() > 0) {
    attrs += ",\"capture_mode\":\"" + jsonEscape(captureMode) + "\"";
  }

  String f = "{\"geometry\":{\"x\":" + String(lon, 6) +
             ",\"y\":" + String(lat, 6) +
             ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{" +
             attrs + "}}";
  return f;
}

String buildPolygonFeature(const String &line) {
  int p1 = line.indexOf('|');            if (p1 < 0) return "";
  int p2 = line.indexOf('|', p1 + 1);    if (p2 < 0) return "";
  int p3 = line.indexOf('|', p2 + 1);    if (p3 < 0) return "";

  String weed      = line.substring(0, p1);
  String coordStr  = line.substring(p1 + 1, p2);
  String epochStr  = line.substring(p2 + 1, p3);

  // control_type (5th) and gps_accuracy_m (6th) are later additions; older
  // records have fewer fields and must still upload.
  int p4 = line.indexOf('|', p3 + 1);
  String jobNumber, controlType, accuracyStr;
  if (p4 < 0) {
    jobNumber = line.substring(p3 + 1);
  } else {
    jobNumber = line.substring(p3 + 1, p4);
    int p5 = line.indexOf('|', p4 + 1);
    if (p5 < 0) { controlType = line.substring(p4 + 1); }
    else        { controlType = line.substring(p4 + 1, p5);
                  accuracyStr = line.substring(p5 + 1); }
  }

  // Static so these do not land on the stack.
  double *xs = shapeXs;      // shared buffers - see the note at MAX_POLY_VERTICES
  double *ys = shapeYs;
  int n = 0;

  int start = 0;
  while (start < (int)coordStr.length()) {
    if (n >= MAX_POLY_VERTICES) {
      Serial.println("Warning: polygon exceeded MAX_POLY_VERTICES, truncating.");
      break;
    }
    // Tolerate a final vertex with no trailing ';'
    int semi = coordStr.indexOf(';', start);
    int end  = (semi < 0) ? (int)coordStr.length() : semi;

    String pt = coordStr.substring(start, end);
    int comma = pt.indexOf(',');
    if (comma > 0) {
      double x = pt.substring(0, comma).toDouble();       // longitude
      double y = pt.substring(comma + 1).toDouble();      // latitude
      bool valid = !(x == 0.0 && y == 0.0) &&
                   y >= -90.0 && y <= 90.0 && x >= -180.0 && x <= 180.0;
      if (valid) { xs[n] = x; ys[n] = y; n++; }
    }

    if (semi < 0) break;
    start = semi + 1;
  }

  if (n < 3) return "";

  // Strip GPS jitter before anything else looks at the shape - but only where
  // there is enough shape to strip.
  //
  // Douglas-Peucker discards any vertex closer than the tolerance to the line
  // between its neighbours. On a ring whose whole extent is comparable to that
  // tolerance, EVERY vertex qualifies, so smoothing stops being noise removal
  // and becomes demolition. Golden dodder patches run about a metre across
  // against a 2 m default tolerance, which is well inside that.
  //
  // So measure the ring first and leave small ones alone. Their outline is
  // mostly receiver noise either way; that is a limit of the hardware, not
  // something a smoothing pass can repair, and an untouched ring at least
  // reports honestly.
  double minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
  for (int i = 1; i < n; i++) {
    if (xs[i] < minX) minX = xs[i];
    if (xs[i] > maxX) maxX = xs[i];
    if (ys[i] < minY) minY = ys[i];
    if (ys[i] > maxY) maxY = ys[i];
  }
  double spanY = (maxY - minY) * 111320.0;
  double spanX = (maxX - minX) * 111320.0 * cos(ys[0] * 0.017453292519943295);
  double ringSpanM = (spanX > spanY) ? spanX : spanY;

  if (polySimplifyM > 0.0 && ringSpanM < polySimplifyM * 4.0) {
    Serial.println("Polygon " + String(ringSpanM, 1) + "m across - too small for a " +
                   String(polySimplifyM, 1) + "m tolerance, smoothing skipped.");
  } else {
    int before = n;
    n = simplifyPolyline(xs, ys, n, (double)polySimplifyM);
    if (n != before) {
      Serial.println("Polygon smoothed: " + String(before) + " -> " + String(n) + " vertices.");
    }
  }

  // ArcGIS requires outer rings in CLOCKWISE order; counter-clockwise is read
  // as a hole. The GPS track direction depends on which way the operator drove,
  // so normalise it here using the shoelace formula.
  double area2 = 0.0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    area2 += (xs[i] * ys[j]) - (xs[j] * ys[i]);
  }
  bool needsReverse = (area2 > 0.0);   // positive == counter-clockwise

  String rings = "[[";
  rings.reserve(n * 26 + 32);
  for (int k = 0; k < n; k++) {
    int i = needsReverse ? (n - 1 - k) : k;
    if (k > 0) rings += ",";
    rings += "[" + String(xs[i], 6) + "," + String(ys[i], 6) + "]";
  }
  int first = needsReverse ? (n - 1) : 0;   // explicitly close the ring
  rings += ",[" + String(xs[first], 6) + "," + String(ys[first], 6) + "]";
  rings += "]]";

  String esriTimeMs = epochStr + "000";

  String attrs = "\"weed_species\":\"" + jsonEscape(weed) + "\"," +
                 "\"treatment_time\":" + esriTimeMs + "," +
                 "\"job_id\":\"" + jsonEscape(String(currentJobID)) + "\"," +
                 "\"Job_Number\":\"" + jsonEscape(jobNumber) + "\"";
  if (controlType.length() > 0) {
    attrs += ",\"control_type\":\"" + jsonEscape(controlType) + "\"";
  }
  if (accuracyStr.length() > 0) {
    double a = accuracyStr.toDouble();
    if (a > 0.0) attrs += ",\"gps_accuracy_m\":" + String(a, 2);
  }

  String f = "{\"geometry\":{\"rings\":" + rings +
             ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{" +
             attrs + "}}";
  return f;
}

// A polyline. Same stored record as a polygon, and the same jitter smoothing,
// but the ring is never closed and winding order is meaningless - ArcGIS only
// enforces clockwise outer rings on polygons.
String buildLineFeature(const String &line) {
  int p1 = line.indexOf('|');            if (p1 < 0) return "";
  int p2 = line.indexOf('|', p1 + 1);    if (p2 < 0) return "";
  int p3 = line.indexOf('|', p2 + 1);    if (p3 < 0) return "";

  String weed      = line.substring(0, p1);
  String coordStr  = line.substring(p1 + 1, p2);
  String epochStr  = line.substring(p2 + 1, p3);

  int p4 = line.indexOf('|', p3 + 1);
  String jobNumber, controlType, accuracyStr;
  if (p4 < 0) {
    jobNumber = line.substring(p3 + 1);
  } else {
    jobNumber = line.substring(p3 + 1, p4);
    int p5 = line.indexOf('|', p4 + 1);
    if (p5 < 0) { controlType = line.substring(p4 + 1); }
    else        { controlType = line.substring(p4 + 1, p5);
                  accuracyStr = line.substring(p5 + 1); }
  }

  double *lxs = shapeXs;     // same buffers as the polygon builder
  double *lys = shapeYs;
  int n = 0;

  int start = 0;
  while (start < (int)coordStr.length()) {
    if (n >= MAX_POLY_VERTICES) {
      Serial.println("Warning: line exceeded MAX_POLY_VERTICES, truncating.");
      break;
    }
    int semi = coordStr.indexOf(';', start);
    int end  = (semi < 0) ? (int)coordStr.length() : semi;

    String pt = coordStr.substring(start, end);
    int comma = pt.indexOf(',');
    if (comma > 0) {
      double x = pt.substring(0, comma).toDouble();
      double y = pt.substring(comma + 1).toDouble();
      bool valid = !(x == 0.0 && y == 0.0) &&
                   y >= -90.0 && y <= 90.0 && x >= -180.0 && x <= 180.0;
      if (valid) { lxs[n] = x; lys[n] = y; n++; }
    }

    if (semi < 0) break;
    start = semi + 1;
  }

  if (n < 2) return "";

  // Smoothing is skipped on a short line for the same reason it is skipped on
  // a small ring: with only a couple of vertices every one of them sits within
  // tolerance of the line between its neighbours, and the shape disappears.
  if (polySimplifyM > 0.0 && n > 2) {
    int before = n;
    n = simplifyPolyline(lxs, lys, n, (double)polySimplifyM);
    if (n < 2) { n = before; }      // never simplify a line out of existence
    else if (n != before) {
      Serial.println("Line smoothed: " + String(before) + " -> " + String(n) + " vertices.");
    }
  }

  // Length, summed over the geometry AS UPLOADED - after smoothing, not before.
  //
  // The two differ slightly, and it matters which one is stored: anyone who
  // measures the line in ArcGIS Pro is measuring the smoothed shape, and an
  // attribute that disagreed with the map would be the thing they stopped
  // trusting. Summing short geodesic steps also avoids the trap in the obvious
  // alternative - a hosted layer in Web Mercator reports Shape__Length with a
  // scale error of about 20% at this latitude.
  double lengthM = 0.0;
  for (int i = 1; i < n; i++) {
    lengthM += metresBetween(lys[i - 1], lxs[i - 1], lys[i], lxs[i]);
  }

  String paths = "[[";
  paths.reserve(n * 26 + 32);
  for (int i = 0; i < n; i++) {
    if (i > 0) paths += ",";
    paths += "[" + String(lxs[i], 6) + "," + String(lys[i], 6) + "]";
  }
  paths += "]]";

  String esriTimeMs = epochStr + "000";
  String attrs = "\"weed_species\":\"" + jsonEscape(weed) + "\"," +
                 "\"treatment_time\":" + esriTimeMs + "," +
                 "\"job_id\":\"" + jsonEscape(String(currentJobID)) + "\"," +
                 "\"Job_Number\":\"" + jsonEscape(jobNumber) + "\"";
  if (controlType.length() > 0) {
    attrs += ",\"control_type\":\"" + jsonEscape(controlType) + "\"";
  }
  if (accuracyStr.length() > 0) {
    double a = accuracyStr.toDouble();
    if (a > 0.0) attrs += ",\"gps_accuracy_m\":" + String(a, 2);
  }
  attrs += ",\"line_length_m\":" + String(lengthM, 1);

  String f = "{\"geometry\":{\"paths\":" + paths +
             ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{" +
             attrs + "}}";
  return f;
}

// ------------------------------------------------------------------
// 11e. POST one batch, with retries
// ------------------------------------------------------------------
bool postBatch(WiFiClientSecure &client, const char *url,
               const String &featuresJson, int count) {
  // rollbackOnFailure makes ArcGIS apply the batch all-or-nothing, which is
  // what makes retrying below safe.
  String body = "f=json&rollbackOnFailure=true&features=";
  body += urlEncode("[" + featuresJson + "]");
  // Optional ArcGIS API key / token. Anonymous when blank.
  if (esriToken.length() > 0) {
    body += "&token=";
    body += urlEncode(esriToken);
  }

  for (int attempt = 1; attempt <= 3; attempt++) {
    HTTPClient http;
    http.setTimeout(20000);
    http.setConnectTimeout(15000);

    if (!http.begin(client, url)) {
      Serial.println("http.begin() failed");
      delay(1000L * attempt);
      continue;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (esriBatchOk(code, resp, count)) {
      Serial.println("Batch of " + String(count) + " accepted.");
      return true;
    }

    // A NEGATIVE code is the ESP32 HTTP client, not the server: -3 is
    // send-payload-failed and -5 is connection-lost, both of which usually
    // mean memory rather than anything wrong at the far end.
    Serial.println("Batch attempt " + String(attempt) + " failed (HTTP " +
                   String(code) + ", free heap " + String(ESP.getFreeHeap()) +
                   "): " + resp.substring(0, 200));
    if (attempt < 3) delay(1000L * attempt);
  }
  return false;
}

// ------------------------------------------------------------------
// 11f. Sync one pending file
// ------------------------------------------------------------------
// Which geometry the queued records carry. This was a bool while there were
// only two shapes; a third makes the bool a lie.
//
// The PARAMETER below is a plain uint8_t rather than this enum, deliberately.
// Arduino generates prototypes for every function in a .ino and inserts them
// above the sketch, so a user-defined type in a signature is referenced before
// it exists and the build fails with "'GeomKind' has not been declared". The
// enumerators still give the call sites and the switch their names.
enum GeomKind { GEOM_POINT, GEOM_POLYGON, GEOM_LINE };

// Returns true if the file was fully synced and removed.
// On failure, the file is rewritten containing ONLY unsynced records and
// 'counter' is updated to how many remain.
bool syncPendingFile(const char *path, const char *tmpPath, const char *url,
                     uint8_t kind, int &counter, WiFiClientSecure &client) {

  // A queue with nowhere to post is not an error: the records stay on the card
  // and go up once the layer URL is set. Silently "succeeding" would be worse -
  // it would clear the counter and make a backlog look like a clean sync.
  if (url == NULL || url[0] == '\0') {
    if (SD.exists(path)) {
      Serial.println(String("No service URL configured for ") + path +
                     " - holding records on the card.");
      return false;
    }
    counter = 0;
    return true;
  }

  if (!SD.exists(path)) { counter = 0; return true; }

  File in = SD.open(path, FILE_READ);
  if (!in) {
    Serial.println(String("Could not open ") + path);
    return false;
  }

  String batchJson  = "";   // comma-separated feature objects
  String batchRaw   = "";   // the original lines behind batchJson, for rollback
  int    batchCount = 0;
  bool   failed     = false;
  int    sentTotal  = 0;

  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    String feature;
    const char *badTag;
    switch (kind) {
      case GEOM_POLYGON: feature = buildPolygonFeature(line); badTag = "BAD_POLYGON"; break;
      case GEOM_LINE:    feature = buildLineFeature(line);    badTag = "BAD_LINE";    break;
      default:           feature = buildPointFeature(line);   badTag = "BAD_POINT";   break;
    }
    if (feature.length() == 0) {
      quarantineLine(badTag, line);
      continue;   // dropped from pending, preserved in /rejected.csv
    }

    if (batchCount > 0) batchJson += ",";
    batchJson += feature;
    batchRaw  += line;
    batchRaw  += "\n";
    batchCount++;

    if (batchCount >= MAX_BATCH_FEATURES || (int)batchJson.length() >= MAX_BATCH_BYTES) {
      if (!postBatch(client, url, batchJson, batchCount)) { failed = true; break; }
      sentTotal += batchCount;
      batchJson = ""; batchRaw = ""; batchCount = 0;
    }
  }

  // Flush the trailing partial batch.
  if (!failed && batchCount > 0) {
    if (postBatch(client, url, batchJson, batchCount)) {
      sentTotal += batchCount;
      batchJson = ""; batchRaw = ""; batchCount = 0;
    } else {
      failed = true;
    }
  }

  if (!failed) {
    in.close();
    SD.remove(path);
    counter = 0;
    Serial.println(String(path) + ": " + String(sentTotal) + " records synced, file cleared.");
    return true;
  }

  // --- Rollback: keep only what has NOT been accepted ---
  // batchRaw holds the failed batch; 'in' is positioned at everything after it.
  Serial.println(String(path) + ": failed after " + String(sentTotal) +
                 " records. Rewriting pending file with the remainder.");

  SD.remove(tmpPath);
  File out = SD.open(tmpPath, FILE_WRITE);
  int kept = 0;

  if (out) {
    if (batchRaw.length() > 0) { out.print(batchRaw); kept += batchCount; }
    while (in.available()) {
      String l = in.readStringUntil('\n');
      l.trim();
      if (l.length() > 0) { out.println(l); kept++; }
    }
    out.close();
    in.close();
    SD.remove(path);
    if (!SD.rename(tmpPath, path)) {
      Serial.println("WARNING: rename of temp file failed.");
    }
  } else {
    // Could not open a temp file. Leave the original untouched so nothing is
    // lost -- a later retry may re-send the records already accepted.
    Serial.println("WARNING: could not open temp file; pending file left as-is.");
    in.close();
    kept = counter;
  }

  counter = kept;
  return false;
}

// ------------------------------------------------------------------
// 11g. Entry point
// ------------------------------------------------------------------
void syncStoredData() {
  Serial.println("\n--- Starting Sync ---");
  // Printed because a shortage here does not announce itself: TLS simply stops
  // being able to open a connection, and the upload failures look like network
  // faults. Below about 40 KB free, suspect memory before suspecting ArcGIS.
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");

  if (!sdReady) {
    lastSyncStatus = "NO SD CARD";
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi(15000)) {
      Serial.println("Sync Failed: Could not connect to Wi-Fi.");
      lastSyncStatus = "WIFI ERROR - check config";
      return;
    }
  }

  WiFiClientSecure client;
  // Certificate checking is driven by /ca.pem on the SD card. If the file is
  // absent we still sync, but the connection is unverified and we say so.
  if (caCert.length() > 0) {
    client.setCACert(caCert.c_str());
  } else {
    client.setInsecure();
    Serial.println("WARNING: syncing without TLS certificate verification (no /ca.pem).");
  }

  // Points and polygons sync independently -- a failure in one does not
  // prevent the other from being uploaded.
  bool okPts   = syncPendingFile(PENDING_PTS, TMP_PTS,
                                 esriPointServiceURL.c_str(), GEOM_POINT,
                                 unsyncedPoints, client);
  bool okTrack = syncPendingFile(PENDING_TRACK, TMP_TRACK,
                                 esriTrackServiceURL.c_str(), GEOM_POINT,
                                 unsyncedTrack, client);
  bool okPoly  = syncPendingFile(PENDING_POLY, TMP_POLY,
                                 esriPolygonServiceURL.c_str(), GEOM_POLYGON,
                                 unsyncedPolygons, client);
  bool okLine  = syncPendingFile(PENDING_LINE, TMP_LINE,
                                 esriLineServiceURL.c_str(), GEOM_LINE,
                                 unsyncedLines, client);

  if (okPts && okTrack && okPoly && okLine) {
    lastSyncStatus = "SUCCESS!";
    Serial.println("All Points, Track, Polygons and Lines Synced Successfully!");
  } else {
    // Name what is still on the card rather than just which call failed - the
    // number left is what decides whether it is safe to end the day.
    String left = "";
    if (!okPts)   left += String(unsyncedPoints) + " pts ";
    if (!okTrack) left += String(unsyncedTrack) + " trk ";
    if (!okPoly)  left += String(unsyncedPolygons) + " polys ";
    if (!okLine)  left += String(unsyncedLines) + " lines ";
    left.trim();
    lastSyncStatus = "SYNC ERROR - " + left + " left";
  }

  Serial.println("--- Sync finished: " + lastSyncStatus + " ---");
}

// ==========================================
// 12. SETUP
// ==========================================
void setup() {
  // First, before anything else can disturb it: why did we just start?
  recordBoot();

  delay(2500);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- FPE EcoSync Field Booting ---");
  Serial.println("Firmware build: " + String(FIRMWARE_BUILD));
  Serial.println("Restart #" + String(bootCount) + " - reason: " + bootResetReason);
  if (bootWasUnexpected) {
    Serial.println("*** THIS WAS NOT A CLEAN POWER-ON ***");
    Serial.println("    The safety is ON after every restart, so this is what");
    Serial.println("    'the safety came on by itself' actually looks like.");
    if (bootResetCode == (int)ESP_RST_EXT) {
      Serial.println("    EXTERNAL PIN: something pulled EN low. The Fenix pad's");
      Serial.println("    wire is on EN, and EN is the reset line - suspect it first.");
    } else if (bootResetCode == (int)ESP_RST_BROWNOUT) {
      Serial.println("    BROWNOUT: supply dipped. Suspect the battery connector,");
      Serial.println("    a tired cell, or a current spike on Wi-Fi transmit.");
    } else if (bootResetCode == (int)ESP_RST_PANIC) {
      Serial.println("    CRASH: this one is ours. The exception details are in");
      Serial.println("    the serial output immediately BEFORE this banner.");
    }
  }

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setTimeOut(150);

  // --- Battery gauge ---
  if (lipo.begin() == false) {
    batteryOk = false;
    Serial.println("Warning: MAX17048 Battery not detected - battery display disabled.");
  } else {
    batteryOk = true;
    Serial.println("Success: Battery Monitor Connected.");
  }

  // --- SD card (must come before config load) ---
  Serial.print("Accessing SD Card... ");
  SPI.begin(18, 19, 23, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, 10000000)) {
    sdReady = false;
    Serial.println("Failed! Check card. Logging is DISABLED until a card is fitted.");
  } else {
    sdReady = true;
    Serial.println("Ready.");
  }

  // Leaves a record on the card of every restart, so a fault in the paddock
  // with nobody watching is still evidenced afterwards.
  logBootToSD();

  loadConfigFromSD();
  loadLastPoint();

  // AFTER the card is read, not with the build banner above. device_id comes
  // from config.txt, so printing it earlier would have shown the compiled
  // fallback and told you the opposite of the truth on any unit whose identity
  // lives on its card - which, now, is all of them.
  Serial.println("Unit:           " + currentJobID +
                 "   (goes into job_id on every record)");
  Serial.println("Reachable at:   " + mdnsHostname() + ".local  /  AP " + apSsid);

  // --- Lockout recovery -----------------------------------------------------
  // Hold the spray trigger while powering on to clear the config-page password.
  // Physical access already implies access to the SD card, so this grants
  // nothing new - it just avoids needing a card reader in the field.
  {
    bool held = true;
    for (int i = 0; i < 30; i++) {          // must stay held for 3 seconds
      if (digitalRead(TRIGGER_PIN) != LOW) { held = false; break; }
      delay(100);
    }
    if (held) {
      adminPass = "";
      saveConfigToSD();
      Serial.println("*** TRIGGER HELD AT BOOT - config page password CLEARED ***");
      lastAction = "Config password cleared (trigger held at boot)";
    }
  }

  // Drop the clock before anything power-hungry starts. This workload is a 1 Hz
  // GPS poll and some string building; 240 MHz buys nothing and costs ~20 mA.
  setCpuFrequencyMhz(cpuMhz);
  Serial.println("CPU clock set to " + String(getCpuFrequencyMhz()) + " MHz");

  loadCaCertFromSD();
  recoverUnsyncedCounts();

  // After the counts are read, so the recovered ring increments an accurate
  // total rather than being counted twice.
  recoverPolygonJournal();
  currentSlot        = 0;
  currentWeed        = slotWeed[0];
  currentControlType = slotControl[0];
  applySlotTuning();

  // Latch the trigger in hardware. The pin used to be read once per pass of
  // loop(), and a pass can stretch to ~120 ms while the dashboard page is
  // built for a phone - so a quick squeeze that opened and closed inside that
  // window was invisible: no record, no counter, no message. An interrupt
  // cannot be outrun by a slow loop.
  attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), triggerISR, CHANGE);

  // --- GPS ---
  // Try UART first (DAN-F10N), then fall back to I2C (the older M9). A wiring
  // fault on a new module must not cost a day's logging: if UART finds nothing
  // the unit still comes up on whatever is on the I2C bus, and says which.
  // 2 KB of RX buffer, not the 256-byte default. MUST be before begin().
  //
  // This is the bug behind "GPS data stale" with a perfect fix on screen.
  // The module streams NMEA plus UBX NAV-PVT at 1 Hz, and NMEA GSV grows with
  // the satellite count - at 20+ satellites it is well over a kilobyte a
  // second. The loop drains that buffer in pollGPS(), which ran every 200 ms:
  // 256 bytes at ~1.3 KB/s fills in under 200 ms, so the buffer was at its
  // limit by design and any loop pass over that mark overflowed it. An
  // overflow drops bytes mid-frame, the UBX checksum fails, getPVT() returns
  // false, lastGpsUpdate stops advancing - and five seconds later every
  // trigger pull is refused as stale while the dashboard shows 1.3 m and 21
  // satellites, because those came from the last frame that did get through.
  //
  // More satellites makes it worse, which is why a good day in open country
  // was the day it showed up.
  Serial1.setRxBufferSize(2048);
  Serial1.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  bool gnssOk = myGNSS.begin(Serial1);
  if (gnssOk) {
    gnssTransport = "UART1";
    Serial.println("GNSS found on UART1 @ " + String(GNSS_BAUD) +
                   " (RX=" + String(GNSS_RX_PIN) + " TX=" + String(GNSS_TX_PIN) + ")");
  } else {
    Serial.println("No GNSS on UART - falling back to I2C...");
    gnssOk = myGNSS.begin();
    if (gnssOk) {
      gnssTransport = "I2C";
      Serial.println("GNSS found on I2C (the older M9 module).");
    }
  }

  if (!gnssOk) {
    gnssTransport = "none";
    Serial.println("Warning: GPS module not detected on UART or I2C.");
    Serial.println("  Check: TXD->GPIO" + String(GNSS_RX_PIN) +
                   ", RXD->GPIO" + String(GNSS_TX_PIN) + ", 3V3, GND.");
    Serial.println("  TXD/RXD swapped is the usual cause and does no harm.");
  } else {
    // UBX for our own parsing, NMEA for the phone feed. Both share the port.
    // The port constants differ per transport - setting the I2C port while
    // talking over UART compiles cleanly, detects the module, and then sends
    // nothing at all, which looks exactly like a dead module.
    if (gnssTransport == "UART1") myGNSS.setUART1Output(COM_TYPE_UBX | COM_TYPE_NMEA);
    else                         myGNSS.setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA);
    myGNSS.setNMEAOutputPort(nmeaRelay);

    // Which module is actually fitted? PROTVER identifies the generation and
    // therefore whether GST is even supported. Recorded for the diagnostics
    // page so a missing GST can be diagnosed without opening the enclosure.
    if (myGNSS.getProtocolVersion()) {
      gnssProtVer = String(myGNSS.getProtocolVersionHigh()) + "." +
                    String(myGNSS.getProtocolVersionLow());
      Serial.println("GNSS protocol version " + gnssProtVer);
    }

    // --- SouthPAN ---
    // Straight from Geoscience Australia's own receiver configuration guide.
    //
    // The one that matters is USE_TESTMODE. SouthPAN broadcasts with the test
    // mode flag set (SBAS message type 0) and u-blox receivers DISCARD such
    // corrections by default. The module tracks the satellite perfectly well and
    // then bins the corrections - which looks exactly like intermittent SBAS
    // under a clear sky, and cost a day of chasing antenna siting.
    configureSouthPAN();

    // GST carries the accuracy estimate. Without it a phone app shows position
    // but no quality figure, which is exactly what an audit needs recorded.
    enableGst();

    // Silence the module's NMEA when nobody is listening to it.
    //
    // The NMEA feed to phone apps is a RELAY of the receiver's own sentences,
    // so NMEA has to stay on when nmea_out is set. With the feed off, every
    // one of those sentences is pure load on a 38400 baud link shared with
    // the UBX frames the unit actually runs on - roughly two thirds of the
    // traffic, discarded on arrival.
    //
    // Key 0x10740002 is CFG-UART1OUTPROT-NMEA, written as a literal so this
    // does not depend on the library exposing a constant for it. Applied at
    // boot only: changing nmea_out needs a restart to take effect.
    if (!nmeaOut) {
      if (myGNSS.setVal8(0x10740002, 0)) {
        Serial.println("GNSS NMEA output disabled (nmea_out is off) - frees the UART for UBX.");
      } else {
        Serial.println("Could not disable GNSS NMEA output; continuing with it on.");
      }
    }

    myGNSS.setNavigationFrequency(1);
    // Auto-PVT: the module streams position on its own schedule and getPVT()
    // becomes a non-blocking check for fresh data.
    myGNSS.setAutoPVT(true);

    // MSGCONF as well as IOPORT. IOPORT alone persists the UBX+NMEA port
    // setting but NOT the message rates, so a GST enable that worked was lost
    // on the next power cycle and had to be re-sent every boot.
    // NAVCONF too, so the SouthPAN settings survive a power cycle rather than
    // needing to be re-sent on every boot.
    myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT | VAL_CFG_SUBSEC_MSGCONF |
                               VAL_CFG_SUBSEC_NAVCONF);
    Serial.println("Success: GPS Connected (auto-PVT enabled).");
  }

  // --- Networking ---
  // Bring the AP up first when it is meant to be permanent, so the config page
  // is reachable even if the Wi-Fi join then succeeds.
  if (apAlways) startAccessPoint();
  connectWiFi(15000);
  if (MDNS.begin(mdnsHostname().c_str())) {
    Serial.println("mDNS responder started at " + mdnsHostname() + ".local");
  }

  // Start the window clock even when the AP never came up.
  //
  // apStoppedAt is set by stopAccessPoint(), and the periodic window will not
  // open while it is zero. A unit that joins Wi-Fi cleanly at boot never
  // starts an AP at all, so it never stopped one, so no window would EVER
  // open - on exactly the unit that most needs one, because the only other
  // way in is an address you can see on the serial monitor and nowhere else.
  //
  // Found on 002's first boot: joined, no AP, and no window coming.
  if (!apMode && apStoppedAt == 0) {
    apStoppedAt = millis();
    if (apStoppedAt == 0) apStoppedAt = 1;
    if (apWindowMin > 0.0) {
      Serial.println("No access point at boot - first window in " +
                     String(apWindowMin, 0) + " min.");
    }
  }

  // The WebServer only keeps headers it has been told to collect, and these
  // are the ones a browser uses to announce that a request is speculative.
  // The WebServer only retains headers it has been told to collect, and these
  // are the four a browser uses to announce that a request is speculative.
  //
  // This core (3.3.11) declares exactly ONE overload:
  //     void collectHeaders(const char *headerKeys[], const size_t count);
  // There is no variadic form here, so the array is not optional.
  {
    static const char *speculationHeaders[] = {
      "Purpose", "X-Purpose", "X-Moz", "Sec-Purpose"
    };
    server.collectHeaders(speculationHeaders, 4);
  }

  server.on("/",            HTTP_GET,  handleRoot);

  // Arming and disarming is a state change, so it is POST only.
  server.on("/toggleSafety",HTTP_POST, handleToggleSafety);
  // A GET can still arrive from a bookmark, an old link or a scanner. Send it
  // to the dashboard and change nothing - never 404, never act.
  server.on("/toggleSafety",HTTP_GET,  []() {
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/setMode",     HTTP_GET,  handleSetMode);
  server.on("/setTarget",   HTTP_GET,  handleSetTarget);
  server.on("/forceSync",   HTTP_GET,  handleForceSync);
  server.on("/dismiss",     HTTP_GET,  handleDismiss);
  server.on("/map",         HTTP_GET,  handleMap);
  server.on("/linedata",    HTTP_GET,  handleLineData);
  server.on("/config",       HTTP_GET,  handleConfig);        // open to operators
  server.on("/saveConfig",   HTTP_POST, handleSaveConfig);    // field fields only
  server.on("/gstretry",   HTTP_GET,  handleGstRetry);
  server.on("/wifiscan",     HTTP_GET,  handleWifiScan);      // diagnostics, open
  server.on("/advanced",     HTTP_GET,  handleAdvanced);      // password protected
  server.on("/saveAdvanced", HTTP_POST, handleSaveAdvanced);
  Serial.println(systemArmed
                 ? "Ready. System is ARMED."
                 : "Ready. SAFETY IS ON - tap ARM on the dashboard before spraying.");
  server.begin();

  Serial.println(systemArmed
                 ? "System Active and ARMED - trigger will log."
                 : "System Active. SAFETY IS ON - trigger will do NOTHING until armed.");
}

// ==========================================
// 13. MAIN LOOP
// ==========================================
void loop() {
  unsigned long loopStart = millis();
  server.handleClient();
  maintainWiFi();
  maintainNmeaServer();

  if (wifiReconfigured) {
    wifiReconfigured = false;
    connectWiFi(15000);
  }

  if (triggerSync) {
    triggerSync = false;
    syncStoredData();
    lastAction = lastSyncStatus;
  }

  unsigned long currentMillis = millis();

  pollGPS();
  pollBattery();

  // --- Auto-timeout. Deliberately NOT gated on systemArmed: disarming closes
  //     the polygon via the handler, so a polygon can never be left hanging. ---
  // Zero means never. LINE defaults to it, because a bait run stops to refill
  // and a timeout would chop one trail into several - the second press is what
  // ends a line. Without this guard zero would be the WORST case rather than
  // the safest: (now - lastActivity) > 0 is true on almost every pass, so the
  // shape would close immediately after it opened.
  if (polygonActive && polyTimeoutS > 0.0 &&
      (currentMillis - lastTriggerActivity > (unsigned long)(polyTimeoutS * 1000.0))) {
    closePolygon("idle timeout", true);
  }

  samplePolygonVertex();

  // --- SPRAY TRIGGER (PIN 14) ---
  //
  // Edge-latched, debounced by TIME SINCE THE LAST ACCEPTED PRESS.
  //
  // The previous scheme required the pin to still read LOW on a LATER pass of
  // loop(), 100 ms after the change was first seen. That silently assumed the
  // loop was fast compared with a trigger squeeze. It is not always: serving the
  // dashboard, the NMEA relay and the GNSS poll can stretch a pass out, and if
  // the whole press and release happened between two passes, stableTriggerState
  // never changed and the press vanished with no message at all. On 27 Aug the
  // press was seen at 21:21:47.900 and the release at 21:21:48.778 - two
  // consecutive passes 878 ms apart, which is exactly that failure.
  //
  // Acting on the falling edge cannot be defeated by a slow loop: any press the
  // firmware SEES, it acts on.
  int  readTriggerRaw = digitalRead(TRIGGER_PIN);

  // Nothing counts until the pin has been seen settled HIGH once. The hold
  // timer below handles press-to-press re-arming on its own, but it cannot
  // tell a pad resting closed at power-on from a squeeze that began before
  // the unit was listening - so until there has been a genuine release, the
  // trigger stays dead. This is the boot-edge guard, and nothing else.
  if (readTriggerRaw == HIGH) {
    if (triggerHighSince == 0) triggerHighSince = currentMillis;
    if (currentMillis - triggerHighSince >= triggerReleaseSettleMs) {
      triggerSeenReleased = true;
    }
  } else {
    triggerHighSince = 0;      // any dip restarts the settle timer
  }
  bool triggerFired   = false;

  // --- Has a press finished? ---------------------------------------------
  //
  // A closure is over once the pin is sitting HIGH and no edge has arrived for
  // the settle time. The duration comes from the ISR's two timestamps, so it
  // is correct no matter how long the loop took to get here - being late only
  // logs the point late.
  unsigned long heldMs    = 0;
  bool          havePress = false;
  if (readTriggerRaw == HIGH) {
    portENTER_CRITICAL(&triggerMux);
    if (isrFirstLow != 0 &&
        (currentMillis - isrLastEdge) >= triggerReleaseSettleMs) {
      heldMs      = isrLastEdge - isrFirstLow;   // first contact to last release bounce
      isrFirstLow = 0;
      havePress   = true;
    }
    portEXIT_CRITICAL(&triggerMux);
  }

  // triggerSeenReleased is still the boot guard: a pad resting closed at
  // power-on must not log a point before anyone has touched it.
  if (havePress && triggerSeenReleased) {
    if (heldMs >= triggerHoldMs) {
      if (currentMillis - lastAcceptedPress > triggerDebounceDelay) {
        lastAcceptedPress = currentMillis;
        triggerFired      = true;
        triggerAccepted++;
        Serial.println("TRIGGER ACCEPTED #" + String(triggerAccepted) +
                       "  (held " + String(heldMs) + "ms)");
      } else {
        triggerRefused++;
        Serial.println("Trigger press ignored: within the " +
                       String(triggerDebounceDelay) + "ms lockout");
      }
    } else {
      triggerTooBrief++;
      Serial.println("Trigger ignored: held " + String(heldMs) +
                     "ms, needs " + String(triggerHoldMs) + "ms");
    }
  }

  // A HELD trigger is activity too.
  //
  // lastTriggerActivity used to be touched only by an accepted PRESS, so the
  // idle timeout measured time since the last edge rather than time since the
  // last spraying. Hold the trigger down for longer than polyTimeoutS - which
  // is ordinary when working a patch - and the polygon closed underneath you
  // mid-spray, with the next pull opening a fresh one. One continuous run came
  // back as two or more fragments, and any fragment short enough fell through
  // the vertex test and vanished.
  if (polygonActive && systemArmed && readTriggerRaw == LOW) {
    lastTriggerActivity = currentMillis;
  }

  // --- Summon the access point ---------------------------------------------
  //
  // Hold the trigger for five seconds with the SAFETY ON.
  //
  // Disarmed means it cannot be confused with spraying, and the trigger is the
  // one control still to hand in the situation this exists for: out of range,
  // AP shut down for idleness, and the dashboard is exactly the thing you do
  // not have. There is no way to ask for it through a screen you cannot reach.
  if (!systemArmed && readTriggerRaw == LOW) {
    if (triggerHeldSince == 0) {
      triggerHeldSince = currentMillis;
    } else if (!apMode && currentMillis - triggerHeldSince > AP_SUMMON_HOLD_MS) {
      startAccessPoint();
      lastAction = "Access point ON: " + apSsid + " / " + WiFi.softAPIP().toString();
      triggerHeldSince = 0;
    }
  } else {
    triggerHeldSince = 0;
  }

  // The INTERRUPT is now the authority on "a press happened". Debouncing stays
  // here rather than in the ISR, which keeps millis() out of interrupt context
  // - the ISR does nothing but raise a flag.
  // Edge count, kept purely as a cross-check: if this climbs while accepted
  // presses do not, the pad is chattering rather than the firmware sleeping.
  if (triggerLatched) {
    triggerLatched = false;
    triggerPresses++;
  }

  if (readTriggerRaw != lastTriggerRaw) {
    lastTriggerDebounce = currentMillis;
    triggerRawChanges++;
    // Deliberately unconditional: this prints even when disarmed, so silence
    // here means the pin is not moving at all. The polled edge count is kept
    // purely as a cross-check - if it drifts from the interrupt count, the pad
    // is bouncing rather than the loop being slow.
    Serial.println("Trigger pin -> " + String(readTriggerRaw == LOW ? "LOW (pressed)" : "HIGH (released)") +
                   "  [changes=" + String(triggerRawChanges) +
                   " presses=" + String(triggerPresses) +
                   " accepted=" + String(triggerAccepted) +
                   " brief=" + String(triggerTooBrief) +
                   " armed=" + String(systemArmed ? "YES" : "NO") +
                   " mode=" + loggingMode +
                   " loop=" + String(loopLastMs) + "ms max=" + String(loopMaxMs) + "ms]");
  }
  lastTriggerRaw     = readTriggerRaw;
  stableTriggerState = readTriggerRaw;   // kept so the dashboard can show it

  // --- CONTINUOUS capture ---------------------------------------------------
  //
  // Level-triggered, and that is the whole point. An edge is a moment, and a
  // moment can fall inside the ~120 ms this loop spends building the dashboard
  // for a phone - which is how squeezes went missing. A trigger held for two
  // seconds cannot be missed by anything sampling at 1 ms, so a pad that drops
  // the occasional clean closure stops mattering.
  //
  // Every point is on the card the instant it is taken. Nothing is held in
  // memory waiting to be "finished", so there is no ring to lose.
  if (loggingMode == "CONTINUOUS") {
    bool held = (readTriggerRaw == LOW);

    if (held && systemArmed) {
      runReleasedAt = 0;          // spraying again; the release hold is moot
      if (currentRunId == 0) {
        if (gpsUsable()) {
          currentRunId      = live_epoch;   // the run's identity IS its start time
          continuousPoints  = 0;
          lastContinuousMs  = 0;            // 0 means "take one now"
          lastContinuousLat = 0.0;
          lastContinuousLon = 0.0;
          lastAction = "RUN STARTED: " + currentWeed;
          Serial.println("--- RUN STARTED id " + String(currentRunId) + " ---");
        } else if (currentMillis - lastContinuousMs > 1000) {
          // Throttled, because this branch is reached on every pass of a 1 ms
          // loop and rebuilding the string that often would churn the heap.
          lastContinuousMs = currentMillis;
          lastRejectReason = gpsRejectReason();
          lastAction = "RUN not started - " + lastRejectReason;
        }
      }

      if (currentRunId != 0) {
        unsigned long everyMs = (unsigned long)(slotA[currentSlot] * 1000.0);
        if (everyMs < 500) everyMs = 500;

        if (lastContinuousMs == 0 || currentMillis - lastContinuousMs >= everyMs) {
          lastContinuousMs = currentMillis;

          if (!gpsUsable()) {
            lastRejectReason = gpsRejectReason();
            lastAction = "RUN: fix unusable - " + lastRejectReason;
          } else {
            // No distance gate. Standing still working one patch is exactly
            // when you want the points - that is the ground being treated.
            if (saveRecordLocally(currentWeed, live_lat, live_lon, live_epoch,
                                  currentControlType, live_accuracy_m,
                                  currentRunId, "CONTINUOUS")) {
              continuousPoints++;
              continuousTotal++;
              lastContinuousLat = live_lat;
              lastContinuousLon = live_lon;
              lastAction = "RUN: " + String(continuousPoints) + " pts, " + currentWeed;
            } else {
              lastAction = "RUN SAVE FAILED - check SD card";
            }
          }
        }
      }
    }
    else if (currentRunId != 0) {
      if (!systemArmed) {
        endContinuousRun("safety engaged");
      } else {
        // THE RELEASE HOLD.
        //
        // Working one dodder patch means letting go and pulling again several
        // times - around a tree, changing direction, repositioning. Those are
        // one infestation and have to share one run_id, or the buffer-and-
        // dissolve downstream turns one patch into five fragments.
        //
        // On 25 September the median run was a SINGLE point: 169 runs for 289
        // points. The spatial merge still collapsed them to 39 sensible
        // treated areas, so the hectares were right - but which squeeze
        // covered which ground was lost, and that is the provenance a
        // compliance record is supposed to carry.
        //
        // Two ways out of a run: the trigger has been up long enough, or you
        // have walked far enough that you are plainly at the next weed.
        if (runReleasedAt == 0) runReleasedAt = currentMillis;

        float holdS = slotB[currentSlot];
        if (holdS < 1.0) holdS = 8.0;

        bool timeUp = (currentMillis - runReleasedAt) > (unsigned long)(holdS * 1000.0);
        bool movedOn = false;
        if (gpsUsable() && continuousPoints > 0) {
          movedOn = metresBetween(lastContinuousLat, lastContinuousLon,
                                  live_lat, live_lon) > RUN_END_MOVE_M;
        }

        if      (timeUp)  endContinuousRun("released");
        else if (movedOn) endContinuousRun("moved on");
      }
    }
  }
  else if (currentRunId != 0) {
    endContinuousRun("mode changed");
  }

  {
    {
      // A pull while disarmed used to do nothing at all - no counter, no
      // message - so a safety left on looked exactly like a broken pad.
      if (triggerFired && !systemArmed) {
        pullsWhileDisarmed++;
        lastAction = "SAFETY ON - " + String(pullsWhileDisarmed) +
                     String(pullsWhileDisarmed == 1 ? " pull" : " pulls") + " not logged";
      }

      // CONTINUOUS is handled above, on the trigger's state. A press edge here
      // would double-count it and, worse, would count a refused fix as a
      // rejected pull once a second.
      // ENDING a line is handled before the fix-quality gate, deliberately.
      // The vertices are already on the card and closing captures no new
      // position, so there is nothing a poor fix could corrupt. Refusing it
      // would leave the operator pressing at a line that will not close, and
      // the longer it stays open the more of the next job it swallows.
      if (triggerFired && systemArmed && loggingMode == "LINE" && polygonActive) {
        closePolygon("ended by operator", true);
      }
      else if (triggerFired && systemArmed && loggingMode != "CONTINUOUS") {

        // Nothing is recorded without a fix we trust.
        if (!gpsUsable()) {
          String why = gpsRejectReason();
          rejectedPulls++;
          lastRejectReason = why;
          lastAction = "NOT LOGGED (" + String(rejectedPulls) + " so far) - " + why;
          Serial.println("Spray trigger ignored: " + why);
        }
        else if (loggingMode == "POINT") {
          // One weed, one record. The lock releases on time OR on movement, so
          // working a big bush stays a single point while the next bush along
          // still logs the moment you get there.
          // Measured in GNSS time, not millis(), for two reasons: millis()
          // restarts at zero on every reset - so a rebooting unit handed out a
          // free un-deduplicated point every time - and it drifts from real
          // time whenever the loop stalls serving the dashboard.
          //
          // A saved point older than LASTPOINT_MAX_AGE_S is ignored, so
          // yesterday's last plant cannot suppress this morning's first.
          bool havePrev = hasLoggedPoint && lastPointEpoch > 0 &&
                          live_epoch >= lastPointEpoch &&
                          (live_epoch - lastPointEpoch) <= LASTPOINT_MAX_AGE_S;
          unsigned long sinceS = havePrev ? (live_epoch - lastPointEpoch) : 0;
          double movedM = havePrev
                        ? metresBetween(lastPointLat, lastPointLon, live_lat, live_lon)
                        : 1e9;

          bool timeReleased = (sinceS >= (unsigned long)pointCooldownS);
          bool moveReleased = (pointMinDistM > 0.0) && (movedM >= pointMinDistM);

          if (!havePrev || timeReleased || moveReleased) {
            lastLogTime = currentMillis;
            if (saveRecordLocally(currentWeed, live_lat, live_lon, live_epoch,
                                  currentControlType, live_accuracy_m,
                                  live_epoch, "POINT")) {
              lastPointLat   = live_lat;
              lastPointLon   = live_lon;
              lastPointEpoch = live_epoch;
              hasLoggedPoint = true;
              pointsLogged++;
              trailAdd(live_lat, live_lon, 0);   // a point stands alone
              saveLastPoint();
              lastAction = "Logged Point: " + currentWeed;
              Serial.println("Spot Spray Logged.");
            } else {
              lastAction = "POINT SAVE FAILED - check SD card";
            }
          } else {
            // Tell the operator why nothing happened, or it reads as a fault.
            unsigned long waitS = (unsigned long)pointCooldownS > sinceS
                                ? (unsigned long)pointCooldownS - sinceS : 0;
            lastAction = "Same plant - already logged (" + String(waitS) + "s or " +
                         String(pointMinDistM - movedM, 1) + "m to go)";
            Serial.println("Trigger suppressed: same plant.");
          }
        }
        else if (loggingMode == "POLYGON") {
          lastTriggerActivity = currentMillis;
          if (!polygonActive) {
            startPolygonAt(live_lat, live_lon, live_epoch);
            lastAction = "STARTED Polygon Mapping";
            Serial.println("--- NEW POLYGON STARTED ---");
          } else {
            Serial.println("Spraying inside polygon (timer reset).");
          }
        }

        // LINE is start-follow-end: the press that begins it is not the same
        // kind of event as the press that ends it. A polygon can rely on the
        // idle timeout because spraying is continuous; a bait trail stops to
        // refill, so only a deliberate second press may end it.
        else if (loggingMode == "LINE") {
          // Only ever the START here - the end is handled above, outside the
          // fix gate. Starting DOES need a fix, because the first vertex is a
          // real position.
          lastTriggerActivity = currentMillis;
          startPolygonAt(live_lat, live_lon, live_epoch);
          lastAction = "STARTED Line - press again to end";
          Serial.println("--- NEW LINE STARTED ---");
        }
      }
    }
  }

  loopLastMs = millis() - loopStart;
  if (loopLastMs > loopMaxMs) loopMaxMs = loopLastMs;

  delay(5);
}
