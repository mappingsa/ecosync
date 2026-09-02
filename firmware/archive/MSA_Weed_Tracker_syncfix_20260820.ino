/*
  ==================================================================================
  Mapping Services Australia - MSA EcoSyncField System
  Production Software: Auto-Timeout Polygon Logic + Web Config + ESRI Sync
  Board: SparkFun Thing Plus - ESP32 WROOM (USB-C)
  Wiring: Spray Trigger (Pressure Pad) -> Pin 14 and GND
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

// --- SYSTEM VARIABLES ---
int unsyncedPoints = 0;
int unsyncedPolygons = 0;
String lastSyncStatus = "Ready for first sync";
bool systemArmed = false; 
String lastAction = "System Booted";
bool triggerSync = false; 

// --- NEW: GLOBAL GPS VARIABLES FOR DASHBOARD ---
bool live_gps_lock = false;
float live_accuracy_m = 0.0;

// --- OPERATIONAL MODE VARIABLES ---
String loggingMode = "POINT"; // Set via dashboard
bool polygonActive = false;
String currentPolygonCoords = "";
int vertexCount = 0;
unsigned long polyStartTime = 0;
unsigned long lastPolySampleTime = 0;
const unsigned long polySampleInterval = 2000; // Drop a GPS vertex every 2 seconds

// --- AUTO-TIMEOUT VARIABLES ---
unsigned long lastTriggerActivity = 0;
const unsigned long polyTimeoutDelay = 15000; // 15 seconds of silence closes the polygon

// --- SPRAY TRIGGER VARIABLES (PIN 14) ---
const int TRIGGER_PIN = 14; 
int lastTriggerRaw = HIGH;         
int stableTriggerState = HIGH;          
unsigned long lastTriggerDebounce = 0; 
const unsigned long triggerDebounceDelay = 100; 
unsigned long lastLogTime = 0;
const unsigned long logCooldown = 5000; 

WebServer server(80);
SFE_MAX1704X lipo(MAX1704X_MAX17048); 

// ==========================================
// 1. STARLINK & ESRI CONFIGURATION
// ==========================================
const char* ssid = "Jimmys"; 
const char* password = "Punter2611";
const char* esriPointServiceURL = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Points/FeatureServer/0/addFeatures";
const char* esriPolygonServiceURL = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services/MSA_Weed_Spray_Polygons/FeatureServer/0/addFeatures";
const char* currentJobID = "MSA_EcoSync_001"; 

// ==========================================
// 2. HARDWARE PINS & OBJECTS
// ==========================================
const int SD_CS_PIN = 5;  
SFE_UBLOX_GNSS myGNSS; 

// ==========================================
// 3. TARGET & JOB VARIABLES
// ==========================================
String target1 = "Target A";
String target2 = "Target B";
String target3 = "Target C";
String currentWeed = ""; 
String currentJobNumber = "NO_JOB_SET"; 

// ==========================================
// 4. BATTERY MONITORING FUNCTION
// ==========================================
int getBatteryPercentage() {
  float percentage = lipo.getSOC(); 
  if (percentage > 100.0) percentage = 100.0;
  if (percentage < 0.0) percentage = 0.0;
  return (int)percentage;
}

// ==========================================
// 5. SETUP
// ==========================================
void setup() {
  delay(2500); 
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- MSA EcoSyncField Booting ---");

  pinMode(TRIGGER_PIN, INPUT_PULLUP); 

  Wire.begin(21, 22); 
  Wire.setTimeOut(150); 
  
  if (lipo.begin() == false) {
    Serial.println("Warning: MAX17048 Battery not detected.");
  } else {
    Serial.println("Success: Battery Monitor Connected.");
  }

  if (myGNSS.begin() == false) {
    Serial.println("Warning: GPS module not detected.");
  } else {
    myGNSS.setI2COutput(COM_TYPE_UBX); 
    myGNSS.saveConfigSelective(VAL_CFG_SUBSEC_IOPORT);
    Serial.println("Success: GPS Connected.");
  }

  Serial.print("Accessing SD Card... ");
  SPI.begin(18, 19, 23, SD_CS_PIN); 
  if (!SD.begin(SD_CS_PIN, SPI, 10000000)) { 
    Serial.println("Failed! Check card.");
  } else {
    Serial.println("Ready.");
    loadTargetsFromSD(); 
  }
  
  currentWeed = target1;
  Serial.println("System Active. Waiting for Safety to be disarmed...");

  // Wi-Fi is permanently ON
  WiFi.begin(ssid, password); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  
  if (MDNS.begin("ecosyncfield")) {
    Serial.println("mDNS responder started at ecosyncfield.local");
  }

  // --- DASHBOARD HTML ---
  server.on("/", []() {
    
    // --- NO I2C CALLS HERE: Read straight from global RAM ---
    bool gpsLock = live_gps_lock;
    float accuracy_m = live_accuracy_m;
    // --------------------------------------------------------

    String bgColor = gpsLock ? "#2ecc71" : "#e74c3c"; 
    if (!systemArmed) bgColor = "#95a5a6"; 

    String statusText = gpsLock ? "GPS LOCKED" : "SEARCHING...";
    if (!systemArmed) statusText = "SAFETY ON";
    if (polygonActive) {
      bgColor = "#d35400";
      statusText = "MAPPING POLYGON...";
    }

    String lockBtnText = systemArmed ? "ENGAGE SAFETY" : "ARM SYSTEM";
    String lockBtnColor = systemArmed ? "#e74c3c" : "#2ecc71";
    
    int batPct = getBatteryPercentage();
    float chargeRate = lipo.getChangeRate(); 
    
    String batColor = (batPct > 20) ? "#f1c40f" : "#e74c3c"; 
    String chargingIcon = (chargeRate > 0.5) ? "⚡ " : ""; 

    // Formatting the accuracy text and color
    String accColor = "#e74c3c"; // Red for poor signal or no fix
    if (gpsLock) {
      if (accuracy_m > 0.0 && accuracy_m <= 1.5) accColor = "#2ecc71"; // Green
      else if (accuracy_m > 1.5 && accuracy_m <= 3.0) accColor = "#f39c12"; // Orange
    }
    String accText = gpsLock ? "&plusmn; " + String(accuracy_m, 2) + " m" : "No Fix";
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>"; 
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<meta http-equiv='refresh' content='2'>"; 
    html += "<style>body { background-color: " + bgColor + "; color: white; text-align: center; font-family: sans-serif; padding-top: 3vh; font-size: 1.1em; font-weight: bold; }</style>";
    html += "</head><body>";
    
    html += "<p style='text-align: right; margin: 0 20px 5px 0; color: " + batColor + "; font-size: 0.9em;'>🔋 " + chargingIcon + String(batPct) + "%</p>";
    
    html += "<h3 style='color:black; margin: 0; font-size: 0.9em; letter-spacing: 1px;'>MSA ECOSYNC FIELD</h3>";
    
    html += "<h1 style='margin-bottom: 5px; margin-top: 5px;'>" + statusText + "</h1>";

    html += "<p style='margin: 0 0 15px 0; font-size: 0.85em; color: white;'>Accuracy: <span style='background-color: " + accColor + "; color: white; padding: 4px 8px; border-radius: 6px; border: 1px solid white;'>" + accText + "</span></p>";

    html += "<a href='/toggleSafety' style='background-color:" + lockBtnColor + "; color:white; padding:10px 20px; text-decoration:none; border-radius:8px; display:inline-block; font-size: 0.9em; border:2px solid white; margin-bottom: 10px;'>" + lockBtnText + "</a><br>";

    html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";
    
    html += "<h3 style='color:black; margin-bottom: 2px;'>Active Target: " + currentWeed + "</h3>";
    html += "<h4 style='color:white; margin-top: 0; margin-bottom: 5px;'>Job Number: " + currentJobNumber + "</h4>"; 
    html += "<h4 style='color:black; margin-top: 0; margin-bottom: 10px;'>Mode: " + loggingMode + (polygonActive ? " (RECORDING)" : "") + "</h4>"; 
    
    String nextMode = (loggingMode == "POINT") ? "POLYGON" : "POINT";
    html += "<a href='/setMode?m=" + nextMode + "' style='background-color:#d35400; color:white; padding:8px 15px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em; margin-bottom:10px;'>Switch to " + nextMode + " Mode</a><br>";

    html += "<div style='margin: 5px 0;'>";
    html += "<a href='/setTarget?w=1' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + target1 + "</a>";
    html += "<a href='/setTarget?w=2' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + target2 + "</a>";
    html += "<a href='/setTarget?w=3' style='background-color:#2980b9; color:white; padding:8px; margin:4px; text-decoration:none; border-radius:5px; display:inline-block; font-size: 0.85em;'>" + target3 + "</a>";
    html += "</div>";
    
    html += "<hr style='width:80%; border-color:white; margin: 10px auto;'>";
    
    html += "<p style='font-size: 0.75em; margin:0;'>Unsynced Points / Polygons:</p>";
    html += "<h2 style='font-size: 2em; margin: 0;'>" + String(unsyncedPoints) + " Pts / " + String(unsyncedPolygons) + " Polys</h2><br>";
    
    html += "<a href='/forceSync' style='background-color:#8e44ad; color:white; padding:12px 25px; text-decoration:none; border-radius:8px; display:inline-block; border:2px solid white; font-size: 0.9em;'>SYNC TO ESRI</a><br><br>";

    html += "<p style='font-size: 0.75em; color:black; margin:0;'>Last Action:</p>";
    html += "<p style='color:black; font-size: 0.9em; margin:3px 0 10px 0;'>" + lastAction + "</p>";
    
    html += "<a href='/config' style='color:white; font-size:1.2em; text-decoration:underline;'>⚙️ Open Device Configuration</a>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  });
  
  // Dashboard Endpoints
  server.on("/toggleSafety", HTTP_GET, []() {
    systemArmed = !systemArmed;
    lastAction = systemArmed ? "System Armed! Ready." : "Safety Engaged.";
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/setMode", HTTP_GET, []() {
    if (server.hasArg("m")) {
      loggingMode = server.arg("m");
      lastAction = "Mode changed to: " + loggingMode;
      polygonActive = false; 
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/setTarget", HTTP_GET, []() {
    if (server.hasArg("w")) {
      int w = server.arg("w").toInt();
      if (w == 1) currentWeed = target1;
      else if (w == 2) currentWeed = target2;
      else if (w == 3) currentWeed = target3;
      lastAction = "Target updated to: " + currentWeed;
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/forceSync", HTTP_GET, []() {
    triggerSync = true; 
    lastAction = "Sync Initiated...";
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // --- WEB CONFIGURATION PORTAL ---
  server.on("/config", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body { background-color: #2c3e50; color: white; text-align: center; font-family: sans-serif; padding-top: 5vh; font-size: 1.1em; }</style>";
    html += "</head><body>";
    html += "<h2>MSA EcoSyncField Configuration</h2>";
    html += "<form action='/saveConfig' method='GET'>";
    
    html += "<label>Target 1:</label><br>";
    html += "<input type='text' name='t1' value='" + target1 + "' style='padding:10px; margin-bottom:15px; width:80%; font-size:1em; text-align:center;'><br>";
    
    html += "<label>Target 2:</label><br>";
    html += "<input type='text' name='t2' value='" + target2 + "' style='padding:10px; margin-bottom:15px; width:80%; font-size:1em; text-align:center;'><br>";
    
    html += "<label>Target 3:</label><br>";
    html += "<input type='text' name='t3' value='" + target3 + "' style='padding:10px; margin-bottom:15px; width:80%; font-size:1em; text-align:center;'><br>";
    
    html += "<label>Job Number (ID):</label><br>";
    html += "<input type='text' name='job' value='" + currentJobNumber + "' style='padding:10px; margin-bottom:25px; width:80%; font-size:1em; text-align:center;'><br>";
    
    html += "<input type='submit' value='SAVE TO SD CARD' style='background-color:#27ae60; color:white; padding:15px 30px; font-size:1em; border:none; border-radius:8px;'>";
    html += "</form>";
    
    html += "<br><br><a href='/' style='color:#bdc3c7; text-decoration:none;'>Cancel & Return to Dashboard</a>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  });

  server.on("/saveConfig", HTTP_GET, []() {
    if (server.hasArg("t1") && server.hasArg("t2") && server.hasArg("t3") && server.hasArg("job")) {
      target1 = server.arg("t1");
      target2 = server.arg("t2");
      target3 = server.arg("t3");
      currentJobNumber = server.arg("job");

      currentWeed = target1; 

      SD.remove("/config.txt");
      File configFile = SD.open("/config.txt", FILE_WRITE);
      if (configFile) {
        configFile.println(target1);
        configFile.println(target2);
        configFile.println(target3);
        configFile.println(currentJobNumber);
        configFile.close();
      }
      lastAction = "Config Saved to SD Card!";
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });
  
  server.begin();
}

// ==========================================
// 6. MAIN LOOP
// ==========================================
void loop() {
  server.handleClient(); 
  
  if (triggerSync) {
    triggerSync = false; 
    syncStoredData();
    lastAction = lastSyncStatus; 
  }

  unsigned long currentMillis = millis();

  // --- LIVE GPS POSITION & ACCURACY (NON-BLOCKING) ---
  static unsigned long lastGpsPoll = 0;
  
  // Bumped to 1000ms so we aren't hammering the I2C bus
  if (currentMillis - lastGpsPoll >= 1000) {
    lastGpsPoll = currentMillis;

    // Passing (10) to EVERY function forces a strict 10-millisecond timeout
    if (myGNSS.getPVT(10)) { 
      
      // Horizontal accuracy estimate 
      uint32_t acc_mm = myGNSS.getHorizontalAccEst(10);
      live_accuracy_m = acc_mm / 1000.0;
      
      // Update the dashboard lock status
      live_gps_lock = myGNSS.getGnssFixOk(10);

      // Print to Serial Monitor for live debugging
      Serial.print("GPS Accuracy: ");
      Serial.print(live_accuracy_m, 2);
      Serial.print(" m | Satellites: ");
      Serial.print(myGNSS.getSIV(10));
      Serial.print(" | Fix: ");
      Serial.println(myGNSS.getFixType(10));
    }
  }

  // --- AUTO-TIMEOUT LOGIC FOR POLYGONS ---
  if (polygonActive && systemArmed && (currentMillis - lastTriggerActivity > polyTimeoutDelay)) {
    polygonActive = false;
    
    if (vertexCount >= 3) {
      savePolygonLocally(currentWeed, currentPolygonCoords, polyStartTime);
      lastAction = "Auto-Saved Polygon (" + String(vertexCount) + " pts)";
      Serial.println("--- 15s TIMEOUT REACHED: POLYGON SAVED ---");
    } else {
      lastAction = "Polygon Cancelled (Timeout - too few points)";
      Serial.println("--- 15s TIMEOUT REACHED: POLYGON CANCELLED ---");
    }
  }

  // --- POLYGON ACTIVE BACKGROUND VERTEX SAMPLING ---
  if (polygonActive && systemArmed) {
    if (currentMillis - lastPolySampleTime >= polySampleInterval) {
      lastPolySampleTime = currentMillis;
      double lat = myGNSS.getLatitude() / 10000000.0;
      double lon = myGNSS.getLongitude() / 10000000.0;
      if (lat != 0.0 && lon != 0.0) {
        currentPolygonCoords += String(lon, 6) + "," + String(lat, 6) + ";";
        vertexCount++;
      }
    }
  }

  // --- SPRAY TRIGGER LOGIC (PIN 14) ---
  int readTriggerRaw = digitalRead(TRIGGER_PIN);

  if (readTriggerRaw != lastTriggerRaw) {
    lastTriggerDebounce = currentMillis;
  }

  if ((currentMillis - lastTriggerDebounce) > triggerDebounceDelay) {
    if (readTriggerRaw != stableTriggerState) {
      stableTriggerState = readTriggerRaw; 

      if (stableTriggerState == LOW && systemArmed) { 
        
        if (loggingMode == "POINT") {
          if (currentMillis - lastLogTime > logCooldown) {
            lastLogTime = currentMillis; 
            Serial.println("Spot Spray Logged!");
            
            double currentLat = myGNSS.getLatitude() / 10000000.0;
            double currentLon = myGNSS.getLongitude() / 10000000.0;
            unsigned long currentEpoch = myGNSS.getUnixEpoch(); 
            
            saveRecordLocally(currentWeed, currentLat, currentLon, currentEpoch);
            lastAction = "Logged Point: " + currentWeed;
          }
        } 
        else if (loggingMode == "POLYGON") {
          lastTriggerActivity = currentMillis; 

          if (!polygonActive) {
            polygonActive = true;
            currentPolygonCoords = "";
            vertexCount = 0;
            polyStartTime = myGNSS.getUnixEpoch();
            
            double startLat = myGNSS.getLatitude() / 10000000.0;
            double startLon = myGNSS.getLongitude() / 10000000.0;
            currentPolygonCoords += String(startLon, 6) + "," + String(startLat, 6) + ";";
            vertexCount = 1;
            lastPolySampleTime = currentMillis;
            
            lastAction = "STARTED Polygon Mapping";
            Serial.println("--- NEW POLYGON STARTED ---");
          } else {
            Serial.println("Spraying inside polygon (Timer reset to 15s).");
          }
        }
      } 
    }
  }
  lastTriggerRaw = readTriggerRaw;
  
  delay(10); 
}

// ==========================================
// 7. SD CARD FUNCTIONS
// ==========================================
void loadTargetsFromSD() {
  if (SD.exists("/config.txt")) {
    File configFile = SD.open("/config.txt", FILE_READ);
    if (configFile) {
      target1 = configFile.readStringUntil('\n'); target1.trim();
      target2 = configFile.readStringUntil('\n'); target2.trim();
      target3 = configFile.readStringUntil('\n'); target3.trim();
      currentJobNumber = configFile.readStringUntil('\n'); currentJobNumber.trim();
      if (currentJobNumber == "") currentJobNumber = "MSA_Job_001";
      configFile.close();
    }
  } else {
    File configFile = SD.open("/config.txt", FILE_WRITE);
    if (configFile) {
      configFile.println(target1);
      configFile.println(target2);
      configFile.println(target3);
      configFile.println("MSA_Job_001");
      configFile.close();
      currentJobNumber = "MSA_Job_001";
    }
  }
}

void saveRecordLocally(String weed, double lat, double lon, unsigned long epochTime) {
  File dataFile = SD.open("/pending.csv", FILE_APPEND);
  if (dataFile) {
    dataFile.println(weed + "," + String(lat, 6) + "," + String(lon, 6) + "," + String(epochTime) + "," + currentJobNumber);
    dataFile.close();
    unsyncedPoints++;
  }
}

void savePolygonLocally(String weed, String coords, unsigned long epochTime) {
  File dataFile = SD.open("/pending_poly.csv", FILE_APPEND);
  if (dataFile) {
    dataFile.println(weed + "|" + coords + "|" + String(epochTime) + "|" + currentJobNumber);
    dataFile.close();
    unsyncedPolygons++;
  }
}

// ==========================================
// 8. ECOSYNC TO ESRI SYNC
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

// Largest polygon we will attempt to upload, in vertices.
// At the 2s sample interval this is a little over 30 minutes of continuous spraying.
const int MAX_POLY_VERTICES  = 1000;

// ------------------------------------------------------------------
// 8a. Encoding helpers
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
  static const char *HEX = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3 + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in.charAt(i);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 0x0F];
    }
  }
  return out;
}

// ------------------------------------------------------------------
// 8b. Quarantine for unusable records
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
// 8c. Response validation
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
// 8d. Feature builders. Return "" if the record is unusable.
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
  String jobNumber = line.substring(c4 + 1);

  // Reject null island and out-of-range coordinates.
  if (lat == 0.0 && lon == 0.0) return "";
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return "";

  String esriTimeMs = epochStr + "000";

  String f = "{\"geometry\":{\"x\":" + String(lon, 6) +
             ",\"y\":" + String(lat, 6) +
             ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{" +
             "\"weed_species\":\"" + jsonEscape(weed) + "\"," +
             "\"treatment_time\":" + esriTimeMs + "," +
             "\"job_id\":\"" + jsonEscape(String(currentJobID)) + "\"," +
             "\"Job_Number\":\"" + jsonEscape(jobNumber) + "\"}}";
  return f;
}

String buildPolygonFeature(const String &line) {
  int p1 = line.indexOf('|');            if (p1 < 0) return "";
  int p2 = line.indexOf('|', p1 + 1);    if (p2 < 0) return "";
  int p3 = line.indexOf('|', p2 + 1);    if (p3 < 0) return "";

  String weed      = line.substring(0, p1);
  String coordStr  = line.substring(p1 + 1, p2);
  String epochStr  = line.substring(p2 + 1, p3);
  String jobNumber = line.substring(p3 + 1);

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

  String f = "{\"geometry\":{\"rings\":" + rings +
             ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{" +
             "\"weed_species\":\"" + jsonEscape(weed) + "\"," +
             "\"treatment_time\":" + esriTimeMs + "," +
             "\"job_id\":\"" + jsonEscape(String(currentJobID)) + "\"," +
             "\"Job_Number\":\"" + jsonEscape(jobNumber) + "\"}}";
  return f;
}

// ------------------------------------------------------------------
// 8e. POST one batch, with retries
// ------------------------------------------------------------------
bool postBatch(WiFiClientSecure &client, const char *url,
               const String &featuresJson, int count) {
  // rollbackOnFailure makes ArcGIS apply the batch all-or-nothing, which is
  // what makes retrying below safe.
  String body = "f=json&rollbackOnFailure=true&features=";
  body += urlEncode("[" + featuresJson + "]");

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
// 8f. Sync one pending file
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
// 8g. Entry point
// ------------------------------------------------------------------
void syncStoredData() {
  Serial.println("\n--- Starting Sync ---");

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sync Failed: Could not connect to Wi-Fi.");
    lastSyncStatus = "WIFI ERROR";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();   // TODO: replace with client.setCACert() for production

  // Points and polygons sync independently -- a failure in one does not
  // prevent the other from being uploaded.
  bool okPts  = syncPendingFile("/pending.csv", "/pending.tmp",
                                esriPointServiceURL, false, unsyncedPoints, client);
  bool okPoly = syncPendingFile("/pending_poly.csv", "/pending_poly.tmp",
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
