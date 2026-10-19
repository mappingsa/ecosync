// =========================================================================
//  MSA EcoSync - DAN-F10N raw measurement test
//
//  ONE QUESTION: does this module output CARRIER PHASE?
//
//  UBX-RXM-RAWX is defined in the F10/M10 firmware interface description,
//  but that document covers every F10 and M10 chip. u-blox gates carrier
//  phase to one chip variant (UBX-F10050-KB-RW), and the DAN-F10N datasheet
//  never mentions carrier phase, raw measurements, RAWX or RINEX anywhere in
//  its feature tables. So the message existing on the platform says nothing
//  about this module emitting it.
//
//  Results come out three ways, because the test has to be run OUTSIDE and a
//  laptop on the end of a USB cable in the yard is a nuisance:
//
//    1. Serial Monitor at 115200
//    2. The unit's own access point  ->  SSID  EcoSync-RAWX-Test
//                                        pass  rawxtest
//                                        page  http://192.168.4.1
//    3. /rawx_test.txt written to the SD card at the end
//
//  THIS REPLACES THE FIELD FIRMWARE. Run flash.bat afterwards.
//  Nothing is written to the GNSS module's flash - every setting is applied
//  to RAM only and clears on a power cycle. The SD card is only written at
//  the very end, and only to /rawx_test.txt.
//
//  WIRING - same as the field unit:
//      breakout TXD -> GPIO16      breakout RXD -> GPIO17
//      breakout 3V3 -> 3V3         GND -> GND       (NOT the 5V pin)
// =========================================================================

#include <Arduino.h>
#include <stdarg.h>      // sayf() takes varargs; do not rely on Arduino.h for it
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>

const int  GNSS_RX_PIN = 16;
const int  GNSS_TX_PIN = 17;
const long GNSS_BAUD   = 38400;
const int  SD_CS_PIN   = 5;

// Two minutes, not one. You have to carry the unit outside, wake your phone,
// join the access point and open the page before the verdict lands.
const unsigned long LISTEN_MS = 120000;

const char *AP_SSID = "EcoSync-RAWX-Test";
const char *AP_PASS = "rawxtest";          // 8 chars - WPA2 minimum

// --- Configuration keys (u-blox generic configuration, RAM layer) --------
const uint32_t KEY_RXM_RAWX_UART1  = 0x209102a5;
const uint32_t KEY_RXM_SFRBX_UART1 = 0x20910232;
const uint32_t KEY_UART1_OUT_NMEA  = 0x10740002;

WebServer server(80);

// --- Tally ---------------------------------------------------------------
unsigned long rawxFrames = 0, sfrbxFrames = 0, otherFrames = 0, badChecksum = 0;
unsigned long measSeen = 0, measWithCp = 0;
bool          printedOneFrame = false;
bool          sdReady = false;
bool          finished = false;
unsigned long testStart = 0;

// Everything printed is kept here so the web page and the SD file can show
// the same thing the Serial Monitor did. Capped so a long run cannot eat the
// heap out from under the web server.
String report;
const size_t REPORT_MAX = 14000;

void say(const String &line) {
  Serial.println(line);
  if (report.length() < REPORT_MAX) {
    report += line;
    report += '\n';
  }
}

void sayf(const char *fmt, ...) {
  char buf[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  say(String(buf));
}

// =========================================================================
//  UBX framing
// =========================================================================

void ubxSend(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t head[4] = { cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t ca = 0, cb = 0;
  for (int i = 0; i < 4; i++) { ca += head[i]; cb += ca; }
  for (uint16_t i = 0; i < len; i++) { ca += payload[i]; cb += ca; }

  Serial1.write(0xB5); Serial1.write(0x62);
  Serial1.write(head, 4);
  if (len) Serial1.write(payload, len);
  Serial1.write(ca); Serial1.write(cb);
  Serial1.flush();
}

int waitAck(uint8_t cls, uint8_t id, unsigned long timeoutMs) {
  unsigned long start = millis();
  uint8_t  buf[64];
  int      state = 0;
  uint16_t len = 0, got = 0;
  uint8_t  mCls = 0, mId = 0;

  while (millis() - start < timeoutMs) {
    if (!Serial1.available()) { delay(1); continue; }
    uint8_t c = Serial1.read();
    switch (state) {
      case 0: if (c == 0xB5) state = 1; break;
      case 1: state = (c == 0x62) ? 2 : 0; break;
      case 2: mCls = c; state = 3; break;
      case 3: mId = c;  state = 4; break;
      case 4: len = c;  state = 5; break;
      case 5: len |= ((uint16_t)c << 8); got = 0;
              state = (len > sizeof(buf)) ? 0 : 6; break;
      case 6:
        buf[got++] = c;
        if (got >= len) {
          if (mCls == 0x05 && len >= 2 && buf[0] == cls && buf[1] == id) {
            return (mId == 0x01) ? 1 : 0;
          }
          state = 0;
        }
        break;
    }
  }
  return -1;
}

bool setValU1(uint32_t key, uint8_t value, const char *label) {
  uint8_t p[9];
  p[0] = 0x00;
  p[1] = 0x01;            // RAM layer only - nothing reaches the module's flash
  p[2] = 0x00; p[3] = 0x00;
  p[4] = (uint8_t)(key      ); p[5] = (uint8_t)(key >>  8);
  p[6] = (uint8_t)(key >> 16); p[7] = (uint8_t)(key >> 24);
  p[8] = value;

  ubxSend(0x06, 0x8A, p, sizeof(p));
  int r = waitAck(0x06, 0x8A, 1500);

  sayf("  %-24s key 0x%08lX -> %s", label, (unsigned long)key,
       r == 1 ? "ACK" : r == 0 ? "NAK (key not supported)" : "no reply");
  return r == 1;
}

// =========================================================================
//  RXM-RAWX decoding
// =========================================================================
//
// Header 16 bytes, then one 32-byte block per measurement:
//   prMes R8 | cpMes R8 | doMes R4 | gnssId U1 | svId U1 | sigId U1 |
//   freqId U1 | locktime U2 | cno U1 | prStdev X1 | cpStdev X1 |
//   doStdev X1 | trkStat X1 | reserved U1
//
// trkStat bit 0 = pseudorange valid, bit 1 = CARRIER PHASE VALID.

double readR8(const uint8_t *p) { double d; memcpy(&d, p, 8); return d; }

const char *gnssName(uint8_t id) {
  switch (id) {
    case 0: return "GPS";    case 1: return "SBAS";  case 2: return "Galileo";
    case 3: return "BeiDou"; case 5: return "QZSS";  case 6: return "GLONASS";
    default: return "?";
  }
}

void decodeRawx(const uint8_t *p, uint16_t len) {
  if (len < 16) return;
  rawxFrames++;

  double   rcvTow  = readR8(p);
  uint16_t week    = p[8] | (p[9] << 8);
  uint8_t  numMeas = p[11];

  bool showThis = !printedOneFrame;
  if (showThis) {
    say("");
    say("  --- first RXM-RAWX frame -------------------------------");
    sayf("  rcvTow %.3f   week %u   numMeas %u   payload %u bytes",
         rcvTow, week, numMeas, len);
    say("   gnss      sv sig cno        prMes (m)     cpMes (cycles) lock trk");
  }

  for (uint8_t i = 0; i < numMeas; i++) {
    uint16_t off = 16 + (uint16_t)i * 32;
    if (off + 32 > len) break;

    double   prMes    = readR8(p + off);
    double   cpMes    = readR8(p + off + 8);
    uint8_t  gnssId   = p[off + 20];
    uint8_t  svId     = p[off + 21];
    uint8_t  sigId    = p[off + 22];
    uint16_t locktime = p[off + 24] | (p[off + 25] << 8);
    uint8_t  cno      = p[off + 26];
    uint8_t  trkStat  = p[off + 30];

    bool cpValid = (trkStat & 0x02) && (cpMes != 0.0);

    measSeen++;
    if (cpValid) measWithCp++;

    if (showThis && i < 10) {
      sayf("  %-8s %3u %3u %3u %16.3f %16.3f %5u 0x%02X%s",
           gnssName(gnssId), svId, sigId, cno, prMes, cpMes,
           locktime, trkStat, cpValid ? " CP" : "");
    }
  }

  if (showThis) {
    if (numMeas > 10) sayf("  ... %u more measurements", numMeas - 10);
    say("  --------------------------------------------------------");
    printedOneFrame = true;
  }
}

// =========================================================================
//  Stream reader
// =========================================================================

void pump() {
  static int      state = 0;
  static uint8_t  mCls = 0, mId = 0;
  static uint16_t len = 0, got = 0;
  static uint8_t  buf[2048];
  static uint8_t  ckA = 0, ckB = 0;

  while (Serial1.available()) {
    uint8_t c = Serial1.read();
    switch (state) {
      case 0: if (c == 0xB5) state = 1; break;
      case 1: state = (c == 0x62) ? 2 : 0; break;
      case 2: mCls = c; ckA = c; ckB = ckA; state = 3; break;
      case 3: mId  = c; ckA += c; ckB += ckA; state = 4; break;
      case 4: len = c;  ckA += c; ckB += ckA; state = 5; break;
      case 5: len |= ((uint16_t)c << 8); ckA += c; ckB += ckA; got = 0;
              if (len > sizeof(buf)) { state = 0; break; }
              state = len ? 6 : 7; break;
      case 6:
        buf[got++] = c; ckA += c; ckB += ckA;
        if (got >= len) state = 7;
        break;
      case 7:
        if (c != ckA) { badChecksum++; state = 0; break; }
        state = 8;
        break;
      case 8:
        if (c != ckB) { badChecksum++; state = 0; break; }
        if      (mCls == 0x02 && mId == 0x15) decodeRawx(buf, len);
        else if (mCls == 0x02 && mId == 0x13) sfrbxFrames++;
        else if (mCls == 0x0A && mId == 0x04) {
          say("  MON-VER:");
          if (len >= 30) {
            char t[31];
            memcpy(t, buf, 30); t[30] = 0;        sayf("    sw: %s", t);
            memcpy(t, buf + 30, 10); t[10] = 0;   sayf("    hw: %s", t);
            for (uint16_t e = 40; e + 30 <= len; e += 30) {
              memcpy(t, buf + e, 30); t[30] = 0;  sayf("    %s", t);
            }
          }
        }
        else otherFrames++;
        state = 0;
        break;
    }
  }
}

// =========================================================================
//  Web page
// =========================================================================

String verdictText() {
  if (!finished) return "";
  if (rawxFrames == 0) {
    return "NO RAWX AT ALL.\n"
           "This module does not emit raw measurements. Post-processing is off\n"
           "the table on this hardware - it would need a receiver built on the\n"
           "carrier-phase variant, or an F9-class module.";
  }
  if (measWithCp == 0) {
    return "RAWX YES, CARRIER PHASE NO.\n"
           "Pseudorange and Doppler only. Code-only post-processing could take\n"
           "1.5 m to somewhere under a metre. Centimetres are out.";
  }
  return "CARRIER PHASE PRESENT.\n"
         "PPK is worth pursuing. The next questions are whether phase lock\n"
         "survives a gun-mounted patch antenna, and how far the nearest CORS\n"
         "station is.";
}

String escapeHtml(const String &in) {
  String o;
  o.reserve(in.length() + 32);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if      (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

void handleRoot() {
  unsigned long elapsed = (millis() - testStart) / 1000;
  unsigned long total   = LISTEN_MS / 1000;

  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  if (!finished) h += "<meta http-equiv='refresh' content='3'>";
  h += "<title>EcoSync RAWX test</title><style>";
  h += "body{background:#1b2838;color:#ecf0f1;font-family:sans-serif;margin:0;padding:12px;}";
  h += "h1{font-size:1.1em;margin:0 0 2px 0;color:#2ecc71;}";
  h += ".sub{font-size:0.75em;color:#95a5a6;margin-bottom:12px;}";
  h += ".box{background:#22303f;border-radius:8px;padding:10px;margin-bottom:10px;}";
  h += ".n{font-size:1.6em;font-weight:bold;}";
  h += ".row{display:flex;justify-content:space-between;font-size:0.85em;padding:2px 0;}";
  h += ".k{color:#95a5a6;}";
  h += "pre{font-size:0.62em;line-height:1.35;white-space:pre-wrap;word-break:break-word;margin:0;}";
  h += ".good{color:#2ecc71;} .bad{color:#e74c3c;} .warn{color:#f39c12;}";
  h += "</style></head><body>";

  h += "<h1>EcoSync RAWX test</h1>";
  h += "<div class='sub'>Does the DAN-F10N output carrier phase?</div>";

  h += "<div class='box'>";
  if (!finished) {
    h += "<div class='row'><span class='k'>Running</span><span>";
    h += String(elapsed) + " / " + String(total) + " s</span></div>";
  } else {
    h += "<div class='row'><span class='k'>Status</span><span class='good'>Complete</span></div>";
  }
  h += "<div class='row'><span class='k'>RXM-RAWX frames</span><span>" + String(rawxFrames) + "</span></div>";
  h += "<div class='row'><span class='k'>RXM-SFRBX frames</span><span>" + String(sfrbxFrames) + "</span></div>";
  h += "<div class='row'><span class='k'>Measurements</span><span>" + String(measSeen) + "</span></div>";
  h += "<div class='row'><span class='k'>With carrier phase</span><span class='";
  h += (measWithCp > 0 ? "good" : "bad");
  h += "'>" + String(measWithCp) + "</span></div>";
  h += "<div class='row'><span class='k'>Bad checksums</span><span class='";
  h += (badChecksum > 5 ? "warn" : "k");
  h += "'>" + String(badChecksum) + "</span></div>";
  h += "</div>";

  if (finished) {
    h += "<div class='box'><div class='n ";
    h += (measWithCp > 0 ? "good" : "bad");
    h += "'>";
    h += (rawxFrames == 0 ? "NO RAWX" : measWithCp == 0 ? "NO CARRIER PHASE" : "CARRIER PHASE");
    h += "</div><pre>" + escapeHtml(verdictText()) + "</pre></div>";
    h += "<div class='box'><div class='k' style='font-size:0.8em;'>";
    h += "Saved to /rawx_test.txt on the SD card";
    if (!sdReady) h += " &mdash; <span class='bad'>SD not available, log is here only</span>";
    h += ". Run flash.bat to restore the field firmware.</div></div>";
  }

  h += "<div class='box'><pre>" + escapeHtml(report) + "</pre></div>";
  h += "</body></html>";

  server.send(200, "text/html", h);
}

void handleRaw() {
  server.send(200, "text/plain", report + "\n" + verdictText() + "\n");
}

// =========================================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  say("=========================================================");
  say(" DAN-F10N raw measurement test");
  say(" Does this module output CARRIER PHASE?");
  say("=========================================================");
  say("");
  say("RAM-only settings. A power cycle undoes everything here.");
  say("");

  // Access point first, so the page is up while the rest runs.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  sayf("Access point: %s / %s", AP_SSID, AP_PASS);
  sayf("Page:         http://%s", WiFi.softAPIP().toString().c_str());
  say("");

  server.on("/", handleRoot);
  server.on("/raw", handleRaw);
  server.begin();

  SPI.begin(18, 19, 23, SD_CS_PIN);
  sdReady = SD.begin(SD_CS_PIN, SPI, 10000000);
  sayf("SD card:      %s", sdReady ? "ready" : "NOT FOUND (results on the page only)");
  say("");

  Serial1.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  delay(400);

  say("Module identity (UBX-MON-VER):");
  ubxSend(0x0A, 0x04, nullptr, 0);
  unsigned long t = millis();
  while (millis() - t < 2000) { pump(); server.handleClient(); }
  say("");

  say("Enabling raw output:");
  // NMEA off first. RAWX is about 1.6 KB/s against a 3.84 KB/s link, so
  // leaving NMEA running risks truncated frames and a false negative.
  setValU1(KEY_UART1_OUT_NMEA,  0, "NMEA output off");
  bool rawxOk = setValU1(KEY_RXM_RAWX_UART1,  1, "RXM-RAWX on UART1");
                setValU1(KEY_RXM_SFRBX_UART1, 1, "RXM-SFRBX on UART1");

  if (!rawxOk) {
    say("");
    say("  NOTE: RAWX was not accepted. That is close to an answer on its");
    say("        own - listening anyway in case it streams regardless.");
  }

  say("");
  sayf("Listening for %lu seconds. The unit needs a clear view of the sky.",
       LISTEN_MS / 1000);
  say("");

  testStart = millis();
}

void loop() {
  server.handleClient();

  if (finished) { delay(5); return; }

  pump();

  static unsigned long lastTick = 0;
  if (millis() - lastTick > 10000) {
    lastTick = millis();
    sayf("  [%3lus] RAWX %lu  SFRBX %lu  meas %lu  with CP %lu  bad ck %lu",
         (millis() - testStart) / 1000, rawxFrames, sfrbxFrames,
         measSeen, measWithCp, badChecksum);
  }

  if (millis() - testStart > LISTEN_MS) {
    finished = true;

    say("");
    say("=========================================================");
    say(" RESULT");
    say("=========================================================");
    sayf("  RXM-RAWX frames    : %lu", rawxFrames);
    sayf("  RXM-SFRBX frames   : %lu", sfrbxFrames);
    sayf("  Measurements seen  : %lu", measSeen);
    sayf("  With carrier phase : %lu", measWithCp);
    sayf("  Bad checksums      : %lu", badChecksum);
    say("");
    say(verdictText());

    if (badChecksum > rawxFrames / 10 && badChecksum > 5) {
      say("");
      say("  WARNING: a lot of bad checksums. The 38400 baud link is probably");
      say("  overflowing, so these counts may understate what the module is");
      say("  actually producing.");
    }
    say("=========================================================");
    say("  Run flash.bat to restore the field firmware.");

    if (sdReady) {
      SD.remove("/rawx_test.txt");
      File f = SD.open("/rawx_test.txt", FILE_WRITE);
      if (f) {
        f.print(report);
        f.close();
        Serial.println("  Written to /rawx_test.txt on the SD card.");
      } else {
        Serial.println("  Could not write /rawx_test.txt.");
      }
    }
  }
}
