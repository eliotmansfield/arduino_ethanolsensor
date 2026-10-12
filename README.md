# Flex Fuel Sensor Monitor

Arduino Mega 2560 project to read and display ethanol content and fuel temperature
from a GM Continental flex fuel sensor (part number 13577429), used on Chevy Equinox,
Silverado, and Tahoe 2015–2018.

---

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | Arduino Mega 2560 |
| Sensor | GM Continental 13577429 |
| Display | 20×4 I2C LCD (PCF8574 backpack) |
| Pull-up resistor | 3.3 kΩ (see critical note below) |

---

## Sensor — GM Continental 13577429

### Electrical connector

Delphi GT150 series, 3-pin. Pigtail available as GM part 13352241.

| Pin | Designation | Function |
|---|---|---|
| 1 | A | 12V battery positive supply |
| 2 | B | Signal ground (common with supply GND) |
| 3 | C | Frequency signal output (open-collector) |

Pin markings are physically printed on the sensor housing. Rear (wire entry) view
mirrors the face view — Pin 1 is on the right when looking into the back of the plug.

### Signal protocol

The output on Pin 3 is an **open-collector frequency signal**. The ECM (or external
pull-up) holds the line high; the sensor pulls it to ground in pulses.

**Ethanol content** is encoded as output frequency:

```
ethanol_percent = frequency_hz - 50
```

| Frequency | Meaning |
|---|---|
| 50 Hz | 0% ethanol (pure petrol) |
| 150 Hz | 100% ethanol (E100) |
| 150–180 Hz | Elevated — possible contamination |
| 180–190 Hz | Fuel contamination confirmed |
| >190 Hz | Sensor fault / out of range |

**Fuel temperature** is encoded in the low-side pulse width (how long the sensor holds
the line LOW per cycle). Because the output is open-collector, the high side is passive
(pull-up); the sensor actively drives the low side:

```
pulse_low_ms  = period_ms - pulse_high_ms
temp_celsius  = (41.5 × pulse_low_ms) - 81.25
```

### Operating specifications

| Parameter | Value |
|---|---|
| Supply voltage (Pin 1) | 12V DC |
| Operating temperature | −40 °C to +150 °C |
| Fluid connections | 3/8" quick-connect both ends |
| Maximum flow rate | 400 LPH |
| Signal pull-up voltage range | 3.3V to 18V |
| Signal pull-up resistor range | **2.2 kΩ to 3.5 kΩ (strict)** |

### Critical pull-up resistor constraint

The pull-up resistor on Pin 3 **must be 2.2 kΩ to 3.5 kΩ**. A 3.3 kΩ resistor is
recommended. Values outside this range (including the commonly cited 10 kΩ) do not
produce a clean signal and the sensor will appear unresponsive or erratic. This is
not a standard open-collector pull-up situation — the resistor value directly affects
signal quality due to the sensor's internal output stage characteristics.

### Flow direction

Flow direction through the sensor does not affect the electrical output. The dielectric
measurement is not directional. The sensor is typically installed on the feed line
(fuel flows through sensor before the rail) but return line installation also works.

### Fluid compatibility for bench testing

The sensor measures **dielectric constant**, calibrated specifically for ethanol/petrol
blends. Other alcohols give incorrect ethanol percentage readings:

- IPA (isopropyl alcohol, 99.9%): reads approximately 125 Hz (~75% on ethanol scale)
  due to lower dielectric constant (≈18) vs ethanol (≈24). Not suitable for calibration.
- Methylated spirits (~95% ethanol): reads approximately 145 Hz. Suitable for testing.
- Petrol (E5 UK): reads approximately 50–55 Hz (4–5% ethanol). Good baseline reference.

For bench testing, use polypropylene (Norm-Ject) or glass/PTFE syringes — rubber
plunger tips are attacked by both petrol and IPA.

---

## Wiring

### Sensor to Arduino Mega

```
Sensor Pin 1 (A) ──────────────────────── 12V supply rail
Sensor Pin 2 (B) ──────────────────────── Arduino GND
                                           (tied to 12V supply GND — common ground essential)
Sensor Pin 3 (C) ──┬──── Arduino D2 (INT0)
                   │
                 3.3 kΩ
                   │
                Arduino 5V
```

Sensor requires 12V supply. The Arduino cannot supply this — use an external 12V PSU
or vehicle rail. The signal line swings between 0V and 5V (pull-up voltage). Do not
apply a 12V pull-up to an Arduino GPIO — maximum input is 5.5V.

### LCD to Arduino Mega

```
LCD VCC ── Arduino 5V
LCD GND ── Arduino GND
LCD SDA ── Arduino pin 20   (Mega I2C data — NOT A4 as on Uno/Nano)
LCD SCL ── Arduino pin 21   (Mega I2C clock — NOT A5 as on Uno/Nano)
```

I2C address: typically **0x27** or **0x3F**. Use the `LCD_ADDRESS.ino` scanner sketch
from the harshkzz/Arduino-LCD20x4-i2c repo to confirm before uploading the main sketch.
Update `LCD_I2C_ADDR` in the sketch if required.

---

## Software

### Libraries

| Library | Source |
|---|---|
| `LiquidCrystal_I2C` | harshkzz/Arduino-LCD20x4-i2c (zip) or Library Manager |
| `Wire` | Arduino built-in |

### Sketch architecture

**Interrupt service routine (ISR)**

Uses `CHANGE` interrupt on D2 (INT0) to capture both rising and falling edges via
`attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), sensorISR, CHANGE)`.

- Rising edge: computes full cycle period (`lastPeriod_us`)
- Falling edge: computes pulse high width (`lastPulseHigh_us`), sets `dataReady` flag

ISR shared variables are `volatile unsigned long`. They are copied under
`noInterrupts()` / `interrupts()` guards in the main loop before use.

**Smoothing**

Rolling average over `SMOOTH_N` (8) samples for both frequency and temperature.
Buffer index wraps with modulo. `bufFull` flag tracks whether the buffer has been
fully populated at least once.

**Display strategy**

Static labels (`Ethanol:`, `Temp:`, `Freq:`, `Status:`) are written once in `setup()`
at col 0–7 and never rewritten, avoiding full-screen clear flicker.

Dynamic value fields occupy col 8–19 (12 chars) and are updated every
`DISPLAY_INTERVAL_MS` (500 ms). Each field is always written as exactly 12 chars
(number + unit + trailing spaces) to overwrite any previous content cleanly.

If no ISR data arrives for `SIGNAL_TIMEOUT_MS` (1000 ms), the value fields are blanked
and `NO SIGNAL` appears on the status row.

**Serial output**

Runs at 115200 baud on the hardware UART. Logs Hz, ethanol %, temperature, and status
on every ISR cycle. Useful for debugging or data logging alongside the LCD display.

### Display layout

```
Col:  0       8          19
      01234567890123456789

Row 0: Ethanol:   54.0 %
Row 1: Temp:      25.0 C
Row 2: Freq:      54.0 Hz
Row 3: Status: OK
```

Value field format (col 8–19, always 12 chars):

| Row | dtostrf width | Unit | Trailing spaces | Total |
|---|---|---|---|---|
| Ethanol | 6 | ` %` (2) | 4 | 12 |
| Temp | 6 | ` C` (2) | 4 | 12 |
| Freq | 6 | ` Hz` (3) | 3 | 12 |
| Status | — | — | left-padded to 12 via `snprintf("%-12s")` | 12 |

Status strings (all ≤12 chars): `OK`, `CAUTION`, `CONTAM.`, `FAULT`, `NO SIGNAL`

### Key constants (top of sketch)

| Constant | Default | Notes |
|---|---|---|
| `SENSOR_PIN` | `2` | Must be 2 or 3 on Mega (INT0/INT1) |
| `LCD_I2C_ADDR` | `0x27` | Change to `0x3F` if display is blank |
| `DISPLAY_INTERVAL_MS` | `500` | LCD refresh rate |
| `SMOOTH_N` | `8` | Increase for slower/smoother response |
| `SIGNAL_TIMEOUT_MS` | `1000` | Time before NO SIGNAL declared |

---

## File structure

```
arduinouno_ethanolsensor.ino — main sketch (Mega 2560 + 20x4 LCD)
README.md                    — this file
```

---

## Known constraints and gotchas

- Pull-up resistor on Pin 3 must be 3.3 kΩ (2.2–3.5 kΩ range). 10 kΩ does not work.
- Mega I2C is on pins 20/21, not A4/A5. The harshkzz repo wiring diagram shows Uno pinout.
- IPA reads low (~125 Hz) due to lower dielectric constant than ethanol. Use meths for
  ethanol-scale testing.
- The sensor must have fuel (not air) in the chamber for a valid reading. Air gives
  erratic or out-of-range frequency. Fill with petrol before powering for bench tests.
- The sensor supply must share a common GND with the Arduino. Floating ground causes
  incorrect or missing signal.
- `dtostrf` is used for float formatting on AVR (Mega). Do not substitute `sprintf`
  with `%f` — it is not supported by default in the AVR `sprintf` implementation.
