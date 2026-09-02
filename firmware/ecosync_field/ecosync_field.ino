/*
  ==================================================================================
  Mapping Services Australia - MSA EcoSync Field System
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

// ==========================================
// 1. CONSTANTS
// ==========================================
const char* CONFIG_PATH   = "/config.txt";
const char* CA_CERT_PATH  = "/ca.pem";
const char* PENDING_PTS   = "/pending.csv";
const char* PENDING_POLY  = "/pending_poly.csv";
const char* TMP_PTS       = "/pending.tmp";
const char* TMP_POLY      = "/pending_poly.tmp";

const int   SD_CS_PIN     = 5;
// Bump this on every build that leaves this machine. It prints at boot and
// shows on the dashboard, so the version actually running is never in doubt.
const char* FIRMWARE_BUILD = "20260827-h edge-latched trigger";

const int   TRIGGER_PIN   = 14;

// Service endpoints. These are not secrets, so they stay in the sketch.
const char* esriPointServiceURL   = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Points/FeatureServer/0/addFeatures";
const char* esriPolygonServiceURL = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Polygons/FeatureServer/0/addFeatures";
const char* currentJobID          = "MSA_EcoSync_001";

// Timings
// polyTimeoutDelay and the point cooldown are configurable - see section 2.
const unsigned long polySampleInterval   = 1000;   // how often we consider a vertex
const unsigned long triggerDebounceDelay = 100;

const unsigned long gpsPollInterval      = 200;    // auto-PVT check rate
const unsigned long gpsStaleAfter        = 5000;   // fix considered stale after this
const unsigned long batteryPollInterval  = 15000;
const unsigned long wifiRetryInterval    = 30000;

// Bounds
const int MAX_POLY_VERTICES = 1000;   // backstop; auto-splits when reached
const int POLY_COORD_RESERVE = 4096;  // pre-allocate to limit heap fragmentation

// ==========================================
// 2. CONFIGURABLE SETTINGS (all loaded from /config.txt)
// ==========================================
String target1          = "Target A";
String target2          = "Target B";
String target3          = "Target C";
String currentJobNumber = "MSA_Job_001";

String wifiSsid   = "";          // blank -> device raises its own access point
String wifiPass   = "";
bool   apAlways   = true;         // keep the setup AP up even when joined to Wi-Fi,
                                 // so the config page is always reachable
String apSsid     = "MSA-EcoSync-Setup";
String apPass     = "msaecosync"; // must be at least 8 chars or the AP is open

String esriToken  = "";          // optional ArcGIS API key / token; blank = anonymous
String adminUser  = "msa";
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

// Control method recorded against each target. These are written to the
// control_type field in ArcGIS. Previously the field was left unset, so ArcGIS
// applied the layer default ("Cut and Swab", inherited from the Survey123 form).
String control1 = "Foliar Spray";
String control2 = "Foliar Spray";
String control3 = "Foliar Spray";

// ==========================================
// 3. RUNTIME STATE
// ==========================================
int    unsyncedPoints   = 0;
int    unsyncedPolygons = 0;
String lastSyncStatus   = "Ready for first sync";
String lastAction       = "System Booted";
bool   systemArmed      = false;
bool   triggerSync      = false;
bool   sdReady          = false;

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
String        currentPolygonCoords   = "";
String        currentPolygonWeed     = "";
String        currentPolygonControl  = "";
int           vertexCount     = 0;
unsigned long polyStartTime   = 0;
unsigned long lastPolySampleTime = 0;
unsigned long lastTriggerActivity = 0;
double        lastVertexLat   = 0.0;
double        lastVertexLon   = 0.0;

// --- Point de-dup state ---
double lastPointLat   = 0.0;
double lastPointLon   = 0.0;
bool   hasLoggedPoint = false;

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
unsigned long triggerPresses    = 0;
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
wifi_power_t txPowerSetting() {
  float d = wifiTxDbm;
  if (d < 2.0)  d = 2.0;
  if (d > 19.5) d = 19.5;
  return (wifi_power_t)(int)(d * 4.0);
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

  if      (k == "target1")      target1          = v;
  else if (k == "target2")      target2          = v;
  else if (k == "target3")      target3          = v;
  else if (k == "job")          currentJobNumber = v;
  else if (k == "wifi_ssid")    wifiSsid         = v;
  else if (k == "wifi_pass")    wifiPass         = v;
  else if (k == "ap_always")    apAlways         = (v == "1" || v == "true" || v == "yes");
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
  else if (k == "wifi_tx_dbm")   wifiTxDbm       = v.toFloat();
  else if (k == "nmea_out")      nmeaOut          = (v == "1" || v == "true" || v == "yes");
  else if (k == "nmea_port")     nmeaPort         = v.toInt();
  else if (k == "control1")     control1         = v;
  else if (k == "control2")     control2         = v;
  else if (k == "control3")     control3         = v;
}

static void sanitiseConfig() {
  if (target1.length() == 0)          target1 = "Target A";
  if (target2.length() == 0)          target2 = "Target B";
  if (target3.length() == 0)          target3 = "Target C";
  if (currentJobNumber.length() == 0) currentJobNumber = "MSA_Job_001";
  if (apSsid.length() == 0)           apSsid = "MSA-EcoSync-Setup";
  if (apPass.length() < 8)            apPass = "msaecosync";   // WPA2 minimum
  if (adminUser.length() == 0)        adminUser = "msa";
  if (control1.length() == 0)         control1 = "Foliar Spray";
  if (control2.length() == 0)         control2 = "Foliar Spray";
  if (control3.length() == 0)         control3 = "Foliar Spray";
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
  if (wifiTxDbm < 2.0 || wifiTxDbm > 19.5)            wifiTxDbm = 8.5;
  if (nmeaPort < 1024 || nmeaPort > 65535)            nmeaPort = 10110;
}

bool saveConfigToSD() {
  if (!sdReady) return false;
  SD.remove(CONFIG_PATH);
  File f = SD.open(CONFIG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("Config save FAILED (could not open file).");
    return false;
  }
  f.println("# MSA EcoSync Field configuration");
  f.println("# One key=value per line. Lines starting with # are ignored.");
  f.println("");
  f.println("target1=" + target1);
  f.println("target2=" + target2);
  f.println("target3=" + target3);
  f.println("job=" + currentJobNumber);
  f.println("");
  f.println("# Control method written to the control_type field in ArcGIS,");
  f.println("# one per target. If control_type uses a coded-value domain, put");
  f.println("# the CODE here, not the description.");
  f.println("control1=" + control1);
  f.println("control2=" + control2);
  f.println("control3=" + control3);
  f.println("");
  f.println("# Wi-Fi. Leave wifi_ssid blank to force setup access-point mode.");
  f.println("wifi_ssid=" + wifiSsid);
  f.println("wifi_pass=" + wifiPass);
  f.println("");
  f.println("# Serve this unit's GNSS position to phone apps (ArcGIS QuickCapture,");
  f.println("# Survey123) over TCP, so they log at the unit's accuracy rather than");
  f.println("# the handset's. In the app choose a Network connection receiver and");
  f.println("# enter this unit's address and port.");
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
  f.println("ap_always=" + String(apAlways ? "1" : "0"));
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
  f.close();
  return true;
}

void loadConfigFromSD() {
  if (!sdReady) {
    Serial.println("No SD card - running on built-in defaults.");
    sanitiseConfig();
    return;
  }

  if (!SD.exists(CONFIG_PATH)) {
    Serial.println("No config.txt - writing defaults.");
    sanitiseConfig();
    saveConfigToSD();
    return;
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
    if (legacyCount > 0) target1          = legacyLines[0];
    if (legacyCount > 1) target2          = legacyLines[1];
    if (legacyCount > 2) target3          = legacyLines[2];
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
  if (unsyncedPoints || unsyncedPolygons) {
    Serial.println("Recovered backlog: " + String(unsyncedPoints) + " points, " +
                   String(unsyncedPolygons) + " polygons pending sync.");
    lastSyncStatus = String(unsyncedPoints) + " pts / " +
                     String(unsyncedPolygons) + " polys awaiting sync";
  }
}

bool saveRecordLocally(String weed, double lat, double lon, unsigned long epochTime,
                       String controlType, double accuracyM) {
  if (!sdReady) return false;
  File dataFile = SD.open(PENDING_PTS, FILE_APPEND);
  if (!dataFile) return false;
  // Commas are stripped so the CSV cannot be split incorrectly.
  weed.replace(",", " ");
  controlType.replace(",", " ");
  String job = currentJobNumber; job.replace(",", " ");
  dataFile.println(weed + "," + String(lat, 6) + "," + String(lon, 6) + "," +
                   String(epochTime) + "," + job + "," + controlType + "," +
                   String(accuracyM, 2));
  dataFile.close();
  unsyncedPoints++;
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
void startPolygonAt(double lat, double lon, unsigned long epoch) {
  polygonActive        = true;
  currentPolygonCoords = "";
  currentPolygonCoords.reserve(POLY_COORD_RESERVE);
  currentPolygonWeed    = currentWeed;          // pin the target at start time
  currentPolygonControl = currentControlType;   // and its control method
  vertexCount          = 0;
  polyStartTime        = epoch;
  lastPolySampleTime   = millis();
  polyWorstAccM        = live_accuracy_m;

  currentPolygonCoords += String(lon, 6) + "," + String(lat, 6) + ";";
  vertexCount   = 1;
  lastVertexLat = lat;
  lastVertexLon = lon;
}

// allowSave=false discards; otherwise a polygon of 3+ vertices is written to SD.
void closePolygon(const char *reason, bool allowSave) {
  if (!polygonActive) return;
  polygonActive = false;

  if (allowSave && vertexCount >= 3) {
    if (savePolygonLocally(currentPolygonWeed, currentPolygonCoords, polyStartTime,
                           currentPolygonControl, polyWorstAccM)) {
      lastAction = "Saved polygon, " + String(vertexCount) + " pts (" + reason + ")";
    } else {
      lastAction = "POLYGON SAVE FAILED - check SD card";
    }
  } else {
    lastAction = "Polygon discarded, " + String(vertexCount) + " pts (" + reason + ")";
  }
  Serial.println("--- POLYGON CLOSED: " + String(reason) +
                 " (" + String(vertexCount) + " vertices) ---");

  currentPolygonCoords = "";
  vertexCount = 0;
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
  vertexCount++;
  lastVertexLat = lat;
  lastVertexLon = lon;

  // Keep the WORST figure, so the stored value is a ceiling for the whole ring.
  if (live_accuracy_m > polyWorstAccM) polyWorstAccM = live_accuracy_m;
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
  if (ok) Serial.println("Setup access point up: " + apSsid + " at " + WiFi.softAPIP().toString());
  else    Serial.println("Failed to start access point.");
}

// Returns true if joined as a station. Falls back to access-point mode so the
// config page is always reachable, even with no credentials on the card.
bool connectWiFi(unsigned long timeoutMs) {
  if (wifiSsid.length() == 0) {
    Serial.println("No Wi-Fi SSID configured.");
    wifiLastError = "no SSID configured";
    startAccessPoint();
    return false;
  }

  // A blocking attempt counts as an attempt, so the background retry does not
  // fire again the moment this one gives up.
  lastWifiAttempt = millis();

  // Keep the AP alive if it is already up; otherwise station-only.
  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.setTxPower(txPowerSetting());
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

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
  if (wifiSsid.length() == 0) return;
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiAttempt < wifiRetryInterval) return;
  lastWifiAttempt = now;

  wifiLastStatus = WiFi.status();
  wifiLastError  = String(wifiStatusName(wifiLastStatus));

  Serial.println("Wi-Fi not connected (" + wifiLastError + ") - retrying " + wifiSsid);
  if (!apMode) WiFi.mode(WIFI_STA);
  WiFi.setTxPower(txPowerSetting());
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

// Accept one phone at a time. A newer connection replaces an older one, so a
// dropped app can simply reconnect without the unit needing a restart.
// Ask the module to emit GST, and remember whether it agreed.
//
// The request is a UBX-CFG-MSG that must be acknowledged. Sending it directly
// after setI2COutput() reconfigures the port underneath the very transaction
// carrying the request, so a single attempt can be lost even on a module that
// supports GST perfectly well. Settle, then retry before believing a refusal.
bool enableGst() {
  gstEnableAttempted = true;
  gstEnableOk = false;
  for (int attempt = 0; attempt < 3 && !gstEnableOk; attempt++) {
    delay(100);
    gstEnableOk = myGNSS.enableNMEAMessage(UBX_NMEA_GST, COM_PORT_I2C);
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
  if (polygonActive) { bgColor = "#d35400"; statusText = "MAPPING POLYGON..."; }

  String lockBtnText  = systemArmed ? "ENGAGE SAFETY" : "ARM SYSTEM";
  String lockBtnColor = systemArmed ? "#e74c3c" : "#2ecc71";

  String batColor = (live_battery_pct > 20) ? "#f1c40f" : "#e74c3c";
  String chargingIcon = (live_charge_rate > 0.5) ? "\xE2\x9A\xA1 " : "";
  String batText = batteryOk ? (chargingIcon + String(live_battery_pct) + "%") : "--%";

  String accColor = "#e74c3c";
  if (usable)       accColor = (accuracy_m <= 1.5) ? "#2ecc71" : "#f39c12";
  else if (gpsLock) accColor = "#f39c12";
  String accText = gpsLock ? "&plusmn; " + String(accuracy_m, 2) + " m" : "No Fix";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<style>body { background-color: " + bgColor + "; color: white; text-align: center; font-family: sans-serif; padding-top: 3vh; font-size: 1.1em; font-weight: bold; }</style>";
  html += "</head><body>";

  html += "<p style='text-align: right; margin: 0 20px 5px 0; color: " + batColor + "; font-size: 0.9em;'>\xF0\x9F\x94\x8B " + batText + "</p>";
  html += "<h3 style='color:black; margin: 0; font-size: 0.9em; letter-spacing: 1px;'>MSA ECOSYNC FIELD</h3>";
  html += "<p style='color:black; margin:0; font-size:0.6em; opacity:0.7;'>" + String(FIRMWARE_BUILD) + "</p>";
  html += "<h1 style='margin-bottom: 5px; margin-top: 5px;'>" + statusText + "</h1>";

  html += "<p style='margin: 0 0 4px 0; font-size: 0.85em; color: white;'>Accuracy: <span style='background-color: " + accColor + "; color: white; padding: 4px 8px; border-radius: 6px; border: 1px solid white;'>" + accText + "</span></p>";
  html += "<p style='margin: 0 0 12px 0; font-size: 0.7em; color: black; font-weight: normal;'>Sats: " + String(live_siv) + " &nbsp; Fix type: " + String(live_fix_type) + " &nbsp; " + (usable ? "LOGGING ENABLED" : "LOGGING BLOCKED") + "</p>";

  html += "<a href='/toggleSafety' style='background-color:" + lockBtnColor + "; color:white; padding:10px 20px; text-decoration:none; border-radius:8px; display:inline-block; font-size: 0.9em; border:2px solid white; margin-bottom: 10px;'>" + lockBtnText + "</a><br>";
  html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";

  html += "<h3 style='color:black; margin-bottom: 2px;'>Active Target: " + htmlEscape(currentWeed) + "</h3>";
  html += "<h4 style='color:white; margin: 0 0 5px 0; font-size:0.8em;'>Control: " + htmlEscape(currentControlType) + "</h4>";
  html += "<h4 style='color:white; margin-top: 0; margin-bottom: 5px;'>Job Number: " + htmlEscape(currentJobNumber) + "</h4>";
  html += "<h4 style='color:black; margin-top: 0; margin-bottom: 10px;'>Mode: " + loggingMode + (polygonActive ? " (RECORDING " + String(vertexCount) + " pts)" : "") + "</h4>";

  String nextMode = (loggingMode == "POINT") ? "POLYGON" : "POINT";
  html += "<a href='/setMode?m=" + nextMode + "' style='background-color:#d35400; color:white; padding:8px 15px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em; margin-bottom:10px;'>Switch to " + nextMode + " Mode</a><br>";

  html += "<div style='margin: 5px 0;'>";
  html += "<a href='/setTarget?w=1' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + htmlEscape(target1) + "<br><span style='font-size:0.75em; opacity:0.85;'>" + htmlEscape(control1) + "</span></a>";
  html += "<a href='/setTarget?w=2' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + htmlEscape(target2) + "<br><span style='font-size:0.75em; opacity:0.85;'>" + htmlEscape(control2) + "</span></a>";
  html += "<a href='/setTarget?w=3' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + htmlEscape(target3) + "<br><span style='font-size:0.75em; opacity:0.85;'>" + htmlEscape(control3) + "</span></a>";
  html += "</div>";

  html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";
  html += "<p style='font-size: 0.75em; margin:0;'>Unsynced Points / Polygons:</p>";
  html += "<h2 style='font-size: 2em; margin: 0;'>" + String(unsyncedPoints) + " Pts / " + String(unsyncedPolygons) + " Polys</h2><br>";
  html += "<a href='/forceSync' style='background-color:#8e44ad; color:white; padding:12px 25px; text-decoration:none; border-radius:8px; display:inline-block; border:2px solid white; font-size: 0.9em;'>SYNC TO ESRI</a><br><br>";

  // Refused pulls must be impossible to miss. The whole product is hands-free,
  // so a message that only appears on a screen nobody looks at is not a warning.
  if (rejectedPulls > 0) {
    html += "<div style='background:#c0392b; color:white; padding:8px; margin:8px auto; "
            "width:86%; border-radius:6px; font-size:0.8em;'>";
    html += "<b>" + String(rejectedPulls) + " trigger pull";
    html += (rejectedPulls == 1 ? " was" : "s were");
    html += " NOT logged</b><br>Last reason: " + htmlEscape(lastRejectReason);
    html += "<br>Raise the accuracy limit under Advanced if this keeps happening.";
    html += "</div>";
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
  html += " &nbsp;|&nbsp; presses since boot: " + String(triggerPresses);
  html += " &nbsp;|&nbsp; loop " + String(loopLastMs) + "ms (max " + String(loopMaxMs) + "ms)";
  html += " &nbsp;|&nbsp; " + String(systemArmed ? "ARMED" : "<b>SAFETY ON</b>");
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

void handleSetMode() {
  if (server.hasArg("m")) {
    String m = server.arg("m");
    if (m == "POINT" || m == "POLYGON") {
      bool closedPolygon = polygonActive;
      if (polygonActive) closePolygon("mode changed", true);
      loggingMode = m;
      // Append rather than overwrite, so the save confirmation is not lost.
      if (closedPolygon) lastAction += " | Mode: " + loggingMode;
      else               lastAction = "Mode changed to: " + loggingMode;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSetTarget() {
  if (server.hasArg("w")) {
    int w = server.arg("w").toInt();
    if      (w == 1) { currentWeed = target1; currentControlType = control1; }
    else if (w == 2) { currentWeed = target2; currentControlType = control2; }
    else if (w == 3) { currentWeed = target3; currentControlType = control3; }
    lastAction = "Target: " + currentWeed + " (" + currentControlType + ")";
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleForceSync() {
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

  html += "<h3>TARGETS &amp; CONTROL METHOD</h3>";
  html += textField("Target 1", "t1", target1, false, "");
  html += textField("Target 1 control method", "c1", control1, false, "");
  html += textField("Target 2", "t2", target2, false, "");
  html += textField("Target 2 control method", "c2", control2, false, "");
  html += textField("Target 3", "t3", target3, false, "");
  html += textField("Target 3 control method", "c3", control3, false, "");

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
  html += roRow("Polygon idle timeout", String(polyTimeoutS, 1) + " s");
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
  if (server.hasArg("t1"))  target1          = argTrimmed("t1");
  if (server.hasArg("t2"))  target2          = argTrimmed("t2");
  if (server.hasArg("t3"))  target3          = argTrimmed("t3");
  if (server.hasArg("c1"))  control1         = argTrimmed("c1");
  if (server.hasArg("c2"))  control2         = argTrimmed("c2");
  if (server.hasArg("c3"))  control3         = argTrimmed("c3");
  if (server.hasArg("job")) currentJobNumber = argTrimmed("job");

  sanitiseConfig();

  // A target rename mid-polygon would otherwise mislabel the saved feature.
  if (polygonActive) closePolygon("config changed", true);
  currentWeed        = target1;
  currentControlType = control1;

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
  html += "<b>Password stored:</b> " + String(wifiPass.length() ? "yes" : "NO") + "<br>";
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
      bool isMine = (wifiSsid.length() > 0 && ssid == wifiSsid);
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
  if (wifiSsid.length() == 0) {
    html += "No network is configured. Set one under Advanced Settings.";
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
    html += "<br>GNSS protocol version: ";
    html += (gnssProtVer.length() ? htmlEscape(gnssProtVer) : String("unknown"));
    html += "<br>GST enable request: ";
    if (!gstEnableAttempted)   html += "not attempted";
    else if (gstEnableOk)      html += "accepted by module";
    else                       html += "<span style='color:#e74c3c;'>REFUSED by module</span>";

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

  html += "<h3>WI-FI</h3>";
  html += textField("Wi-Fi Network (SSID)", "wssid", wifiSsid, false, "Leave blank to stay in setup access-point mode");
  html += secretField("Wi-Fi Password", "wpass", wifiPass.length() > 0, "");

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
          "<div style='font-size:0.85em; color:#bdc3c7; padding:0 8%;'>Recommended. The config page stays reachable even when the unit is on Wi-Fi, so you never depend on finding its address.</div></div>";
  html += textField("AP Name", "apssid", apSsid, false, "");
  html += secretField("AP Password", "appass", apPass.length() > 0, "Minimum 8 characters. Blank keeps the current one.");

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
  html += textField("Polygon idle timeout (s)", "ptime", String(polyTimeoutS, 1), false, "Trigger silence that closes an open polygon");

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
  if (server.hasArg("apssid")) apSsid         = argTrimmed("apssid");
  if (server.hasArg("appass") && argTrimmed("appass").length() > 0) apPass    = argTrimmed("appass");
  if (server.hasArg("token")  && server.arg("token").length()  > 0) esriToken = argTrimmed("token");
  if (server.hasArg("auser"))  adminUser      = argTrimmed("auser");
  if (server.hasArg("apassw") && argTrimmed("apassw").length() > 0) adminPass = argTrimmed("apassw");
  if (server.hasArg("acc"))    gpsMaxAccM     = argTrimmed("acc").toFloat();
  if (server.hasArg("pdist"))  polyMinVertexM = argTrimmed("pdist").toFloat();
  if (server.hasArg("psimp"))  polySimplifyM  = argTrimmed("psimp").toFloat();
  if (server.hasArg("ptime"))  polyTimeoutS   = argTrimmed("ptime").toFloat();
  if (server.hasArg("pcool"))  pointCooldownS = argTrimmed("pcool").toFloat();
  if (server.hasArg("pmind"))  pointMinDistM  = argTrimmed("pmind").toFloat();

  sanitiseConfig();

  if (saveConfigToSD()) lastAction = "Advanced settings saved.";
  else                  lastAction = "CONFIG SAVE FAILED - check SD card";

  if (wifiSsid != oldSsid || wifiPass != oldPass) wifiReconfigured = true;
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

  int m = 0;
  for (int i = 0; i < n; i++) {
    if (keep[i]) { xs[m] = xs[i]; ys[m] = ys[i]; m++; }
  }
  // Never simplify a polygon out of existence.
  return (m >= 3) ? m : n;
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
  String jobNumber, controlType, accuracyStr;
  if (c5 < 0) {
    jobNumber = line.substring(c4 + 1);
  } else {
    jobNumber = line.substring(c4 + 1, c5);
    int c6 = line.indexOf(',', c5 + 1);
    if (c6 < 0) { controlType = line.substring(c5 + 1); }
    else        { controlType = line.substring(c5 + 1, c6);
                  accuracyStr = line.substring(c6 + 1); }
  }

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
  static double xs[MAX_POLY_VERTICES];
  static double ys[MAX_POLY_VERTICES];
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

  // Strip GPS jitter before anything else looks at the shape.
  int before = n;
  n = simplifyPolyline(xs, ys, n, (double)polySimplifyM);
  if (n != before) {
    Serial.println("Polygon smoothed: " + String(before) + " -> " + String(n) + " vertices.");
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

    Serial.println("Batch attempt " + String(attempt) + " failed (HTTP " +
                   String(code) + "): " + resp.substring(0, 200));
    if (attempt < 3) delay(1000L * attempt);
  }
  return false;
}

// ------------------------------------------------------------------
// 11f. Sync one pending file
// ------------------------------------------------------------------
// Returns true if the file was fully synced and removed.
// On failure, the file is rewritten containing ONLY unsynced records and
// 'counter' is updated to how many remain.
bool syncPendingFile(const char *path, const char *tmpPath, const char *url,
                     bool isPolygon, int &counter, WiFiClientSecure &client) {

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

    String feature = isPolygon ? buildPolygonFeature(line) : buildPointFeature(line);
    if (feature.length() == 0) {
      quarantineLine(isPolygon ? "BAD_POLYGON" : "BAD_POINT", line);
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
  bool okPts  = syncPendingFile(PENDING_PTS, TMP_PTS,
                                esriPointServiceURL, false, unsyncedPoints, client);
  bool okPoly = syncPendingFile(PENDING_POLY, TMP_POLY,
                                esriPolygonServiceURL, true, unsyncedPolygons, client);

  if (okPts && okPoly) {
    lastSyncStatus = "SUCCESS!";
    Serial.println("All Points and Polygons Synced Successfully!");
  } else if (!okPts && !okPoly) {
    lastSyncStatus = "SYNC ERROR - " + String(unsyncedPoints) + " pts / " +
                     String(unsyncedPolygons) + " polys left";
  } else if (!okPts) {
    lastSyncStatus = "POINT SYNC ERROR - " + String(unsyncedPoints) + " left";
  } else {
    lastSyncStatus = "POLY SYNC ERROR - " + String(unsyncedPolygons) + " left";
  }

  Serial.println("--- Sync finished: " + lastSyncStatus + " ---");
}

// ==========================================
// 12. SETUP
// ==========================================
void setup() {
  delay(2500);
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- MSA EcoSync Field Booting ---");
  Serial.println("Firmware build: " + String(FIRMWARE_BUILD));

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

  loadConfigFromSD();

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

  loadCaCertFromSD();
  recoverUnsyncedCounts();
  currentWeed        = target1;
  currentControlType = control1;

  // --- GPS ---
  if (myGNSS.begin() == false) {
    Serial.println("Warning: GPS module not detected.");
  } else {
    // UBX for our own parsing, NMEA for the phone feed. Both share the bus.
    myGNSS.setI2COutput(COM_TYPE_UBX | COM_TYPE_NMEA);
    myGNSS.setNMEAOutputPort(nmeaRelay);

    // Which module is actually fitted? PROTVER identifies the generation and
    // therefore whether GST is even supported. Recorded for the diagnostics
    // page so a missing GST can be diagnosed without opening the enclosure.
    if (myGNSS.getProtocolVersion()) {
      gnssProtVer = String(myGNSS.getProtocolVersionHigh()) + "." +
                    String(myGNSS.getProtocolVersionLow());
      Serial.println("GNSS protocol version " + gnssProtVer);
    }

    // GST carries the accuracy estimate. Without it a phone app shows position
    // but no quality figure, which is exactly what an audit needs recorded.
    enableGst();

    myGNSS.setNavigationFrequency(1);
    // Auto-PVT: the module streams position on its own schedule and getPVT()
    // becomes a non-blocking check for fresh data.
    myGNSS.setAutoPVT(true);

    // MSGCONF as well as IOPORT. IOPORT alone persists the UBX+NMEA port
    // setting but NOT the message rates, so a GST enable that worked was lost
    // on the next power cycle and had to be re-sent every boot.
    myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT | VAL_CFG_SUBSEC_MSGCONF);
    Serial.println("Success: GPS Connected (auto-PVT enabled).");
  }

  // --- Networking ---
  // Bring the AP up first when it is meant to be permanent, so the config page
  // is reachable even if the Wi-Fi join then succeeds.
  if (apAlways) startAccessPoint();
  connectWiFi(15000);
  if (MDNS.begin("ecosyncfield")) {
    Serial.println("mDNS responder started at ecosyncfield.local");
  }

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/toggleSafety",HTTP_GET,  handleToggleSafety);
  server.on("/setMode",     HTTP_GET,  handleSetMode);
  server.on("/setTarget",   HTTP_GET,  handleSetTarget);
  server.on("/forceSync",   HTTP_GET,  handleForceSync);
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
  if (polygonActive &&
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
  bool triggerFired   = false;

  if (readTriggerRaw != lastTriggerRaw) {
    lastTriggerDebounce = currentMillis;
    triggerRawChanges++;
    if (readTriggerRaw == LOW) {
      triggerPresses++;
      if (currentMillis - lastAcceptedPress > triggerDebounceDelay) {
        lastAcceptedPress = currentMillis;
        triggerFired = true;
      }
    }
    // Deliberately unconditional: this prints even when disarmed, so silence
    // here means the pin is not moving at all.
    Serial.println("Trigger pin -> " + String(readTriggerRaw == LOW ? "LOW (pressed)" : "HIGH (released)") +
                   "  [changes=" + String(triggerRawChanges) +
                   " presses=" + String(triggerPresses) +
                   " armed=" + String(systemArmed ? "YES" : "NO") +
                   " mode=" + loggingMode +
                   " fired=" + String(triggerFired ? "YES" : "no") +
                   " loop=" + String(loopLastMs) + "ms max=" + String(loopMaxMs) + "ms]");
  }
  lastTriggerRaw     = readTriggerRaw;
  stableTriggerState = readTriggerRaw;   // kept so the dashboard can show it

  {
    {
      if (triggerFired && systemArmed) {

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
          unsigned long sinceLast = currentMillis - lastLogTime;
          double movedM = hasLoggedPoint
                        ? metresBetween(lastPointLat, lastPointLon, live_lat, live_lon)
                        : 1e9;

          bool timeReleased = (sinceLast >= (unsigned long)(pointCooldownS * 1000.0));
          bool moveReleased = (pointMinDistM > 0.0) && (movedM >= pointMinDistM);

          if (!hasLoggedPoint || timeReleased || moveReleased) {
            lastLogTime = currentMillis;
            if (saveRecordLocally(currentWeed, live_lat, live_lon, live_epoch,
                                  currentControlType, live_accuracy_m)) {
              lastPointLat   = live_lat;
              lastPointLon   = live_lon;
              hasLoggedPoint = true;
              lastAction = "Logged Point: " + currentWeed;
              Serial.println("Spot Spray Logged.");
            } else {
              lastAction = "POINT SAVE FAILED - check SD card";
            }
          } else {
            // Tell the operator why nothing happened, or it reads as a fault.
            unsigned long waitS = (unsigned long)pointCooldownS - (sinceLast / 1000);
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
      }
    }
  }

  loopLastMs = millis() - loopStart;
  if (loopLastMs > loopMaxMs) loopMaxMs = loopLastMs;

  delay(5);
}
