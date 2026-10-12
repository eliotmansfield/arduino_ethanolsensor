/*
 * GM Continental Flex Fuel Sensor — 13577429
 * Arduino Mega 2560 + 20x4 I2C LCD
 *
 * ── Sensor wiring (GT150 connector) ─────────────────────────────────────────
 *   Pin 1 (A)  ->  12V supply rail
 *   Pin 2 (B)  ->  Arduino GND  (must be common with 12V supply GND)
 *   Pin 3 (C)  ->  Arduino D2   + 3.3 kΩ pull-up resistor to Arduino 5V
 *                  Pull-up range: 2.2–3.5 kΩ (strict). 10 kΩ does not work.
 *
 * ── LCD wiring (I2C) ─────────────────────────────────────────────────────────
 *   VCC  ->  Arduino 5V
 *   GND  ->  Arduino GND
 *   SDA  ->  Arduino pin 20   (Mega I2C data — NOT A4 as on Uno/Nano)
 *   SCL  ->  Arduino pin 21   (Mega I2C clock — NOT A5 as on Uno/Nano)
 *
 *   I2C address is typically 0x27 or 0x3F.
 *   If the display is blank, run the LCD_ADDRESS scanner sketch from the
 *   harshkzz/Arduino-LCD20x4-i2c repo to find your module's address,
 *   then update LCD_I2C_ADDR below.
 *
 * ── Library required ─────────────────────────────────────────────────────────
 *   LiquidCrystal_I2C  (install from the .zip in the repo, or Library Manager)
 *
 * ── Signal characteristics ───────────────────────────────────────────────────
 *   50–150 Hz  ->  0–100% ethanol   (ethanol% = Hz − 50)
 *   150–180 Hz ->  elevated / possible contamination
 *   180–190 Hz ->  fuel contamination confirmed
 *   >190 Hz    ->  sensor fault / out of range
 *
 * ── Temperature formula ───────────────────────────────────────────────────────
 *   temp °C = (41.5 × pulse_low_ms) − 81.25
 *   pulse_low_ms = period_ms − pulse_high_ms  (sensor is open-drain, pull-up high)
 *
 * ── Display layout ────────────────────────────────────────────────────────────
 *   Col:  0       8          19
 *         Ethanol:   54.0 %
 *         Temp:      25.0 C
 *         Freq:      54.0 Hz
 *         Status: OK
 *
 *   Static labels (col 0–7) are printed once in setup.
 *   Dynamic values (col 8–19, always 12 chars wide) update every 500ms.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Configuration ──────────────────────────────────────────────────────────
#define SENSOR_PIN           2      // Interrupt pin — must be 2 or 3 on Mega
#define LCD_I2C_ADDR      0x27      // Try 0x3F if display stays blank
#define DISPLAY_INTERVAL_MS  500    // LCD update interval in milliseconds
#define SMOOTH_N               8    // Rolling average depth (samples)
#define SIGNAL_TIMEOUT_MS   1000    // Declare no signal after this gap (ms)

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 20, 4);

// ── ISR shared variables ───────────────────────────────────────────────────
volatile unsigned long risingTimestamp  = 0;
volatile unsigned long lastPeriod_us    = 0;    // Full cycle period (µs)
volatile unsigned long lastPulseHigh_us = 0;    // High-side pulse width (µs)
volatile bool          dataReady        = false;

// ── Smoothing buffers ──────────────────────────────────────────────────────
float   freqBuffer[SMOOTH_N];
float   tempBuffer[SMOOTH_N];
uint8_t bufIndex = 0;
bool    bufFull  = false;

// ── Timing ─────────────────────────────────────────────────────────────────
unsigned long lastDisplayUpdate = 0;
unsigned long lastDataTime      = 0;

// ── ISR — fires on every signal edge ──────────────────────────────────────
void sensorISR() {
  unsigned long now = micros();
  if (digitalRead(SENSOR_PIN) == HIGH) {
    // Rising edge: calculate full period
    if (risingTimestamp > 0) {
      lastPeriod_us = now - risingTimestamp;
    }
    risingTimestamp = now;
  } else {
    // Falling edge: calculate pulse high width
    if (risingTimestamp > 0) {
      lastPulseHigh_us = now - risingTimestamp;
      dataReady = true;
    }
  }
}

// ── Rolling average ────────────────────────────────────────────────────────
float rollingAverage(float* buf, uint8_t count) {
  float sum = 0;
  for (uint8_t i = 0; i < count; i++) sum += buf[i];
  return sum / (float)count;
}

// ── Status string — max 12 chars for the LCD value field ──────────────────
const char* fuelStatus(float hz) {
  if (hz < 48.0f)     return "NO SIGNAL";
  if (hz <= 150.0f)   return "OK";
  if (hz <= 179.9f)   return "CAUTION";
  if (hz <= 190.0f)   return "CONTAM.";
  return "FAULT";
}

// ── Write a 12-char value field starting at col 8, on the given row ───────
//    numStr : 6-char wide string produced by dtostrf
//    unit   : unit suffix, e.g. " %", " C", " Hz"
void lcdWriteField(uint8_t row, const char* numStr, const char* unit) {
  lcd.setCursor(8, row);
  lcd.print(numStr);                              // always 6 chars from dtostrf
  lcd.print(unit);                                // 2 or 3 chars
  uint8_t trailing = 12 - 6 - (uint8_t)strlen(unit);
  for (uint8_t i = 0; i < trailing; i++) lcd.print(' ');  // pad to exactly 12
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();

  // Splash screen
  lcd.setCursor(0, 0); lcd.print(" Flex Fuel Monitor  ");
  lcd.setCursor(0, 1); lcd.print("  Sensor 13577429   ");
  lcd.setCursor(0, 2); lcd.print(" Arduino Mega 2560  ");
  lcd.setCursor(0, 3); lcd.print("   Initialising...  ");
  delay(2000);

  // Initialise smoothing buffers to a sane resting state
  for (uint8_t i = 0; i < SMOOTH_N; i++) {
    freqBuffer[i] = 50.0f;   // 0% ethanol
    tempBuffer[i] = 20.0f;   // 20 °C
  }

  // Attach interrupt — CHANGE fires on both rising and falling edges
  pinMode(SENSOR_PIN, INPUT);   // External 3.3 kΩ pull-up to 5V handles the high state
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), sensorISR, CHANGE);

  // Static labels — printed once in setup, never overwritten
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Ethanol:");
  lcd.setCursor(0, 1); lcd.print("Temp:   ");
  lcd.setCursor(0, 2); lcd.print("Freq:   ");
  lcd.setCursor(0, 3); lcd.print("Status: ");

  lastDataTime = millis();   // Begin signal timeout from this point

  Serial.println(F("GM Flex Fuel Sensor — ready"));
}

// ── Main loop ──────────────────────────────────────────────────────────────
void loop() {

  // ── Process ISR data ──────────────────────────────────────────────────
  if (dataReady) {
    noInterrupts();
    unsigned long period_us    = lastPeriod_us;
    unsigned long pulseHigh_us = lastPulseHigh_us;
    dataReady = false;
    interrupts();

    if (period_us > 0) {
      float hz         = 1000000.0f / (float)period_us;
      float pulseLow_ms = (float)(period_us - pulseHigh_us) / 1000.0f;
      float tempC      = (41.5f * pulseLow_ms) - 81.25f;

      freqBuffer[bufIndex] = hz;
      tempBuffer[bufIndex] = tempC;
      bufIndex = (bufIndex + 1) % SMOOTH_N;
      if (bufIndex == 0) bufFull = true;
      lastDataTime = millis();

      // Serial output for logging / verification
      uint8_t n = bufFull ? SMOOTH_N : max((uint8_t)1, bufIndex);
      float avgHz   = rollingAverage(freqBuffer, n);
      float avgTemp = rollingAverage(tempBuffer, n);
      float ethanol = constrain(avgHz - 50.0f, 0.0f, 100.0f);

      Serial.print(F("Hz: "));        Serial.print(avgHz, 1);
      Serial.print(F("  E%: "));      Serial.print(ethanol, 1);
      Serial.print(F("  Temp: "));    Serial.print(avgTemp, 1);
      Serial.print(F("C  Status: ")); Serial.println(fuelStatus(avgHz));
    }
  }

  // ── Update LCD at fixed interval ───────────────────────────────────────
  unsigned long now = millis();
  if (now - lastDisplayUpdate < DISPLAY_INTERVAL_MS) return;
  lastDisplayUpdate = now;

  uint8_t n     = bufFull ? SMOOTH_N : max((uint8_t)1, bufIndex);
  float avgHz   = rollingAverage(freqBuffer, n);
  float avgTemp = rollingAverage(tempBuffer, n);
  float ethanol = constrain(avgHz - 50.0f, 0.0f, 100.0f);
  bool  noSignal = (now - lastDataTime > SIGNAL_TIMEOUT_MS);

  char numStr[7];       // dtostrf: width 6 + null terminator
  char statusField[13]; // status: 12 chars + null

  if (noSignal) {
    // Blank all value fields and show NO SIGNAL on the status row
    lcd.setCursor(8, 0); lcd.print("            ");
    lcd.setCursor(8, 1); lcd.print("            ");
    lcd.setCursor(8, 2); lcd.print("            ");
    lcd.setCursor(8, 3); lcd.print("NO SIGNAL   ");
    return;
  }

  // Row 0 — Ethanol %
  dtostrf(ethanol, 6, 1, numStr);
  lcdWriteField(0, numStr, " %");

  // Row 1 — Fuel temperature
  dtostrf(avgTemp, 6, 1, numStr);
  lcdWriteField(1, numStr, " C");

  // Row 2 — Signal frequency
  dtostrf(avgHz, 6, 1, numStr);
  lcdWriteField(2, numStr, " Hz");

  // Row 3 — Status (left-justified, space-padded to 12 chars)
  snprintf(statusField, 13, "%-12s", fuelStatus(avgHz));
  lcd.setCursor(8, 3);
  lcd.print(statusField);
}
