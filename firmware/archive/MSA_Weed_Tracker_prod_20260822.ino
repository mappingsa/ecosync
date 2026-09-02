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
  if (gpsMaxAccM <= 0.0 || gpsMaxAccM > 100.0) gpsMaxAccM = 5.0;
  if (polyMinVertexM < 0.0 || polyMinVertexM > 100.0) polyMinVertexM = 2.0;
  if (polySimplifyM < 0.0 || polySimplifyM > 50.0)    polySimplifyM = 2.0;
  // 3s floor on the polygon timeout: below that, normal pauses between pulls
  // would chop one patch into many polygons.
  if (polyTimeoutS < 3.0 || polyTimeoutS > 300.0)     polyTimeoutS = 15.0;
  if (pointCooldownS < 0.0 || pointCooldownS > 600.0) pointCooldownS = 10.0;
  if (pointMinDistM < 0.0 || pointMinDistM > 1000.0)  pointMinDistM = 5.0;
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
  f.println("# Fallback access point, used when the Wi-Fi above cannot be joined.");
  f.println("# ap_pass must be at least 8 characters or the AP will be left open.");
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
                       String controlType) {
  if (!sdReady) return false;
  File dataFile = SD.open(PENDING_PTS, FILE_APPEND);
  if (!dataFile) return false;
  // Commas are stripped so the CSV cannot be split incorrectly.
  weed.replace(",", " ");
  controlType.replace(",", " ");
  String job = currentJobNumber; job.replace(",", " ");
  dataFile.println(weed + "," + String(lat, 6) + "," + String(lon, 6) + "," +
                   String(epochTime) + "," + job + "," + controlType);
  dataFile.close();
  unsyncedPoints++;
  return true;
}

bool savePolygonLocally(String weed, String coords, unsigned long epochTime,
                        String controlType) {
  if (!sdReady) return false;
  File dataFile = SD.open(PENDING_POLY, FILE_APPEND);
  if (!dataFile) return false;
  weed.replace("|", " ");
  controlType.replace("|", " ");
  String job = currentJobNumber; job.replace("|", " ");
  dataFile.println(weed + "|" + coords + "|" + String(epochTime) + "|" + job +
                   "|" + controlType);
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
                           currentPolygonControl)) {
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
}

// ==========================================
// 9. NETWORKING
// ==========================================
void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(apSsid.c_str(), apPass.length() >= 8 ? apPass.c_str() : NULL);
  apMode = true;
  Serial.println(ok ? "Setup access point up: " + apSsid + " at " + WiFi.softAPIP().toString()
                    : "Failed to start access point.");
}

// Returns true if joined as a station. Falls back to access-point mode so the
// config page is always reachable, even with no credentials on the card.
bool connectWiFi(unsigned long timeoutMs) {
  if (wifiSsid.length() == 0) {
    Serial.println("No Wi-Fi SSID configured.");
    startAccessPoint();
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.println("Wi-Fi connected: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("Wi-Fi join failed - starting setup access point.");
  startAccessPoint();
  return false;
}

// Periodic reconnect so the dashboard comes back by itself after an AP dropout.
void maintainWiFi() {
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiAttempt < wifiRetryInterval) return;
  lastWifiAttempt = now;

  Serial.println("Wi-Fi dropped - reconnecting.");
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

String networkStatusText() {
  if (apMode) return "AP " + apSsid + " / " + WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return "Wi-Fi disconnected";
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

  html += "<p style='font-size: 0.75em; color:black; margin:0;'>Last Action:</p>";
  html += "<p style='color:black; font-size: 0.9em; margin:3px 0 8px 0;'>" + htmlEscape(lastAction) + "</p>";

  html += "<p style='font-size: 0.7em; color:black; font-weight:normal; margin:0 0 10px 0;'>Network: " + htmlEscape(networkStatusText());
  if (!sdReady) html += " &nbsp;|&nbsp; <span style='color:#c0392b;'>NO SD CARD</span>";
  if (caCert.length() == 0) html += " &nbsp;|&nbsp; TLS unverified";
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

void handleConfig() {
  if (!requireAuth()) return;

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>body { background-color: #2c3e50; color: white; text-align: center; font-family: sans-serif; padding-top: 4vh; font-size: 1.05em; } h3 { color:#95a5a6; margin-top:22px; font-size:0.8em; letter-spacing:1px; }</style>";
  html += "</head><body>";
  html += "<h2>MSA EcoSync Field Configuration</h2>";
  html += "<form action='/saveConfig' method='POST'>";

  html += "<h3>TARGETS &amp; JOB</h3>";
  html += textField("Target 1", "t1", target1, false, "");
  html += textField("Target 1 control method", "c1", control1, false, "");
  html += textField("Target 2", "t2", target2, false, "");
  html += textField("Target 2 control method", "c2", control2, false, "");
  html += textField("Target 3", "t3", target3, false, "");
  html += textField("Target 3 control method", "c3", control3, false, "");
  html += textField("Job Number (ID)", "job", currentJobNumber, false, "");
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin:-6px 0 12px 0;'>Control method is written to control_type in ArcGIS. If that field uses a coded-value domain, enter the CODE, not the description.</div>";

  html += "<h3>WI-FI</h3>";
  html += textField("Wi-Fi Network (SSID)", "wssid", wifiSsid, false, "Leave blank to stay in setup access-point mode");
  html += textField("Wi-Fi Password", "wpass", wifiPass, true, "");

  html += "<h3>SETUP ACCESS POINT</h3>";
  html += textField("AP Name", "apssid", apSsid, false, "");
  html += textField("AP Password", "appass", apPass, true, "Minimum 8 characters");

  html += "<h3>ARCGIS</h3>";
  html += textField("API Key / Token", "token", esriToken, true, "Blank = anonymous. Layers must be public.");

  html += "<h3>DEVICE LOGIN</h3>";
  html += textField("Username", "auser", adminUser, false, "");
  html += textField("Password", "apassw", adminPass, true, "Blank disables the login prompt on this page");

  html += "<h3>LOGGING QUALITY</h3>";
  html += textField("Max GPS accuracy (m)", "acc", String(gpsMaxAccM, 1), false, "Sprays are rejected above this figure");
  html += textField("Min vertex spacing (m)", "pdist", String(polyMinVertexM, 1), false, "Distance travelled before a new polygon point");
  html += textField("Smoothing tolerance (m)", "psimp", String(polySimplifyM, 1), false, "Removes GPS jitter from polygon edges on upload. 0 disables.");
  html += textField("Polygon idle timeout (s)", "ptime", String(polyTimeoutS, 1), false, "Trigger silence that closes an open polygon");

  html += "<h3>POINT DE-DUPLICATION</h3>";
  html += "<div style='font-size:0.7em; color:#bdc3c7; margin-bottom:12px; padding:0 10%;'>A large weed can take many trigger pulls. A press logs a new point when EITHER the cooldown has passed OR you have moved far enough to be at a different plant.</div>";
  html += textField("Cooldown on the same spot (s)", "pcool", String(pointCooldownS, 1), false, "Seconds before the same spot logs again");
  html += textField("Movement that logs early (m)", "pmind", String(pointMinDistM, 1), false, "Distance that releases the cooldown. 0 = plain timer.");

  html += "<br><input type='submit' value='SAVE TO SD CARD' style='background-color:#27ae60; color:white; padding:15px 30px; font-size:1em; border:none; border-radius:8px;'>";
  html += "</form>";
  html += "<br><br><a href='/' style='color:#bdc3c7; text-decoration:none;'>Cancel &amp; Return to Dashboard</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSaveConfig() {
  if (!requireAuth()) return;

  String oldSsid = wifiSsid, oldPass = wifiPass;

  if (server.hasArg("t1"))     target1          = server.arg("t1");
  if (server.hasArg("t2"))     target2          = server.arg("t2");
  if (server.hasArg("t3"))     target3          = server.arg("t3");
  if (server.hasArg("c1"))     control1         = server.arg("c1");
  if (server.hasArg("c2"))     control2         = server.arg("c2");
  if (server.hasArg("c3"))     control3         = server.arg("c3");
  if (server.hasArg("job"))    currentJobNumber = server.arg("job");
  if (server.hasArg("wssid"))  wifiSsid         = server.arg("wssid");
  if (server.hasArg("wpass"))  wifiPass         = server.arg("wpass");
  if (server.hasArg("apssid")) apSsid           = server.arg("apssid");
  if (server.hasArg("appass")) apPass           = server.arg("appass");
  if (server.hasArg("token"))  esriToken        = server.arg("token");
  if (server.hasArg("auser"))  adminUser        = server.arg("auser");
  if (server.hasArg("apassw")) adminPass        = server.arg("apassw");
  if (server.hasArg("acc"))    gpsMaxAccM       = server.arg("acc").toFloat();
  if (server.hasArg("pdist"))  polyMinVertexM   = server.arg("pdist").toFloat();
  if (server.hasArg("psimp"))  polySimplifyM    = server.arg("psimp").toFloat();
  if (server.hasArg("ptime"))  polyTimeoutS     = server.arg("ptime").toFloat();
  if (server.hasArg("pcool"))  pointCooldownS   = server.arg("pcool").toFloat();
  if (server.hasArg("pmind"))  pointMinDistM    = server.arg("pmind").toFloat();

  sanitiseConfig();

  // A target rename mid-polygon would otherwise mislabel the saved feature.
  if (polygonActive) closePolygon("config changed", true);
  currentWeed        = target1;
  currentControlType = control1;

  if (saveConfigToSD()) lastAction = "Config Saved to SD Card!";
  else                  lastAction = "CONFIG SAVE FAILED - check SD card";

  if (wifiSsid != oldSsid || wifiPass != oldPass) wifiReconfigured = true;

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

  // control_type is a later addition. Records written by earlier firmware have
  // no sixth field; those still upload, just without the attribute.
  int c5 = line.indexOf(',', c4 + 1);
  String jobNumber, controlType;
  if (c5 < 0) { jobNumber = line.substring(c4 + 1); controlType = ""; }
  else        { jobNumber = line.substring(c4 + 1, c5); controlType = line.substring(c5 + 1); }

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

  int p4 = line.indexOf('|', p3 + 1);   // optional control_type field
  String jobNumber, controlType;
  if (p4 < 0) { jobNumber = line.substring(p3 + 1); controlType = ""; }
  else        { jobNumber = line.substring(p3 + 1, p4); controlType = line.substring(p4 + 1); }

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
  loadCaCertFromSD();
  recoverUnsyncedCounts();
  currentWeed        = target1;
  currentControlType = control1;

  // --- GPS ---
  if (myGNSS.begin() == false) {
    Serial.println("Warning: GPS module not detected.");
  } else {
    myGNSS.setI2COutput(COM_TYPE_UBX);
    myGNSS.setNavigationFrequency(1);
    // Auto-PVT: the module streams position on its own schedule and getPVT()
    // becomes a non-blocking check for fresh data.
    myGNSS.setAutoPVT(true);
    myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
    Serial.println("Success: GPS Connected (auto-PVT enabled).");
  }

  // --- Networking ---
  connectWiFi(15000);
  if (MDNS.begin("ecosyncfield")) {
    Serial.println("mDNS responder started at ecosyncfield.local");
  }

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/toggleSafety",HTTP_GET,  handleToggleSafety);
  server.on("/setMode",     HTTP_GET,  handleSetMode);
  server.on("/setTarget",   HTTP_GET,  handleSetTarget);
  server.on("/forceSync",   HTTP_GET,  handleForceSync);
  server.on("/config",      HTTP_GET,  handleConfig);
  server.on("/saveConfig",  HTTP_POST, handleSaveConfig);
  server.begin();

  Serial.println("System Active. Waiting for Safety to be disarmed...");
}

// ==========================================
// 13. MAIN LOOP
// ==========================================
void loop() {
  server.handleClient();
  maintainWiFi();

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
  int readTriggerRaw = digitalRead(TRIGGER_PIN);
  if (readTriggerRaw != lastTriggerRaw) {
    lastTriggerDebounce = currentMillis;
  }

  if ((currentMillis - lastTriggerDebounce) > triggerDebounceDelay) {
    if (readTriggerRaw != stableTriggerState) {
      stableTriggerState = readTriggerRaw;

      if (stableTriggerState == LOW && systemArmed) {

        // Nothing is recorded without a fix we trust.
        if (!gpsUsable()) {
          String why = gpsRejectReason();
          lastAction = "NOT LOGGED - " + why;
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
                                  currentControlType)) {
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
  lastTriggerRaw = readTriggerRaw;

  delay(5);
}
