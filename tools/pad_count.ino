/*
  Pad press counter - same logic as the field firmware, on a spare pin.

  Mirrors ecosync_field: FALLING interrupt raises a flag, the loop debounces
  with a 250 ms lockout after an ACCEPTED press, and prints the same counters.
  So the numbers here mean the same thing as the ones from the AER-04 bench run.

  WIRING
    Red          -> GPIO 34
    Black        -> GND
    Shield       -> GND
    10k resistor -> GPIO 34 to 3V3     <-- MANDATORY on 34, it has no internal pull-up

  WHAT TO DO
    Press deliberately 50 times on the BIG PAD. Note the final line.
    Reset, then do 50 on the LO BUTTON.

    changes should be exactly 2x your presses (one down, one up).
    presses is the raw interrupt count - bounce makes it read high.
    accepted is what the firmware would actually log. It should equal 50.
*/

const int  PIN_PAD = 34;
const unsigned long DEBOUNCE_MS = 250;      // matches triggerDebounceDelay
const unsigned long SETTLE_MS   = 60;       // matches triggerReleaseSettleMs

volatile bool latched = false;
void IRAM_ATTR isr() { latched = true; }

unsigned long presses = 0, accepted = 0, changes = 0, lastAccepted = 0;
unsigned long highSince = 0;
bool settled = false;        // pin has been HIGH long enough to re-arm
int lastRaw;

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_PAD, INPUT);                  // external 10k is the pull-up
  lastRaw = digitalRead(PIN_PAD);
  Serial.println();
  Serial.println("--- Pad press counter (GPIO34, 250ms debounce) ---");
  if (lastRaw != HIGH) {
    Serial.println("** Idle is LOW. Either the pad is held, or the 10k pull-up");
    Serial.println("** is not connected and the pin is floating. Fix before counting.");
  }
  attachInterrupt(digitalPinToInterrupt(PIN_PAD), isr, FALLING);
}

void loop() {
  unsigned long now = millis();

  int raw = digitalRead(PIN_PAD);

  // Re-arm only after a SETTLED release. Release chatter on a rubber pad lands
  // after any sane time lockout if the press was held longer than it.
  if (raw == HIGH) {
    if (highSince == 0) highSince = now;
    if (now - highSince >= SETTLE_MS) settled = true;
  } else {
    highSince = 0;
  }

  if (latched) {
    latched = false;
    presses++;
    if (settled && now - lastAccepted > DEBOUNCE_MS) {
      lastAccepted = now;
      settled      = false;
      accepted++;
    }
  }
  if (raw != lastRaw) {
    changes++;
    lastRaw = raw;
    Serial.printf("%-8s  changes=%-4lu presses=%-4lu accepted=%-4lu  (bounce %+ld)\n",
                  raw == LOW ? "PRESSED" : "released",
                  changes, presses, accepted, (long)presses - (long)accepted);
  }
}
