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
// 8. ECOSUNC TO ESRI SYNC FUNCTION
// ==========================================
void syncStoredData() {
  Serial.println("\n--- Starting End of Day Sync ---");
  
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
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nSync Failed: Could not connect to Wi-Fi.");
    lastSyncStatus = "WIFI ERROR";
    return;
  }
  
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;

  // 1. SYNC POINTS (/pending.csv)
  if (SD.exists("/pending.csv")) {
    File dataFile = SD.open("/pending.csv", FILE_READ);
    if (dataFile) {
      while (dataFile.available()) {
        String line = dataFile.readStringUntil('\n');
        line.trim(); 
        if (line.length() > 0) {
          int c1 = line.indexOf(',');
          int c2 = line.indexOf(',', c1 + 1);
          int c3 = line.indexOf(',', c2 + 1);
          int c4 = line.indexOf(',', c3 + 1);

          if (c1 != -1 && c2 != -1 && c3 != -1 && c4 != -1) {
            String weedSpecies = line.substring(0, c1);
            double lat = line.substring(c1 + 1, c2).toDouble();
            double lon = line.substring(c2 + 1, c3).toDouble();
            String epochStr = line.substring(c3 + 1, c4);
            String jobNumber = line.substring(c4 + 1);
            String esriTimeMs = epochStr + "000"; 

            http.begin(client, esriPointServiceURL);
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");

            String featureJson = "[{\"geometry\":{\"x\":" + String(lon, 6) + ",\"y\":" + String(lat, 6) + ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{\"weed_species\":\"" + weedSpecies + "\",\"treatment_time\":" + esriTimeMs + ",\"job_id\":\"" + currentJobID + "\",\"Job_Number\":\"" + jobNumber + "\"}}]";

            int code = http.POST("f=json&features=" + featureJson);
            String resp = http.getString(); 
            http.end();

            if (!(code == 200 && (resp.indexOf("\"success\":true") > 0 || resp.indexOf("\"success\": true") > 0))) {
              Serial.println("Point Sync Error: " + resp);
              lastSyncStatus = "POINT SYNC ERROR";
              dataFile.close();
              return;
            }
          }
        }
      }
      dataFile.close();
      SD.remove("/pending.csv");
      unsyncedPoints = 0;
    }
  }

  // 2. SYNC POLYGONS (/pending_poly.csv)
  if (SD.exists("/pending_poly.csv")) {
    File polyFile = SD.open("/pending_poly.csv", FILE_READ);
    if (polyFile) {
      while (polyFile.available()) {
        String line = polyFile.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          int p1 = line.indexOf('|');
          int p2 = line.indexOf('|', p1 + 1);
          int p3 = line.indexOf('|', p2 + 1);

          if (p1 != -1 && p2 != -1 && p3 != -1) {
            String weedSpecies = line.substring(0, p1);
            String coordStr = line.substring(p1 + 1, p2);
            String epochStr = line.substring(p2 + 1, p3);
            String jobNumber = line.substring(p3 + 1);
            String esriTimeMs = epochStr + "000";

            String ringsJson = "[[";
            int startIdx = 0;
            int semiIdx = coordStr.indexOf(';');
            String firstCoord = "";
            
            while (semiIdx != -1) {
              String pt = coordStr.substring(startIdx, semiIdx);
              if (firstCoord == "") firstCoord = pt;
              ringsJson += "[" + pt + "],";
              startIdx = semiIdx + 1;
              semiIdx = coordStr.indexOf(';', startIdx);
            }
            if (firstCoord != "") {
              ringsJson += "[" + firstCoord + "]";
            }
            ringsJson += "]]";

            http.begin(client, esriPolygonServiceURL);
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");

            String featureJson = "[{\"geometry\":{\"rings\":" + ringsJson + ",\"spatialReference\":{\"wkid\":4326}},\"attributes\":{\"weed_species\":\"" + weedSpecies + "\",\"treatment_time\":" + esriTimeMs + ",\"job_id\":\"" + currentJobID + "\",\"Job_Number\":\"" + jobNumber + "\"}}]";

            int code = http.POST("f=json&features=" + featureJson);
            String resp = http.getString(); 
            http.end();

            if (!(code == 200 && (resp.indexOf("\"success\":true") > 0 || resp.indexOf("\"success\": true") > 0))) {
              Serial.println("Polygon Sync Error: " + resp);
              lastSyncStatus = "POLY SYNC ERROR";
              polyFile.close();
              return;
            }
          }
        }
      }
      polyFile.close();
      SD.remove("/pending_poly.csv");
      unsyncedPolygons = 0;
    }
  }

  lastSyncStatus = "SUCCESS!";
  Serial.println("All Points and Polygons Synced Successfully!");
}