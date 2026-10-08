# Wiring

Board: **ESP32 DevKit** (30/38-pin, 3.3 V logic). Pin numbers below are GPIO numbers
and match the constants at the top of
[`loco_hazard_helmet.ino`](../firmware/loco_hazard_helmet/loco_hazard_helmet.ino).
If you wire differently, change the constants - nothing else.

## Pin map

| Part | Part pin | ESP32 | Notes |
|---|---|---|---|
| GPS module | TX | **GPIO 16** (RX) | Cross-over: GPS *TX* goes to ESP32 *RX* |
| GPS module | RX | **GPIO 17** (TX) | GPS *RX* goes to ESP32 *TX* |
| GPS module | VCC / GND | 3V3 / GND | Most NEO-6M-style boards accept 3.3-5 V; check yours |
| DHT11 | DATA | **GPIO 4** | Bare sensors need a 10 k pull-up to 3V3; 3-pin modules already have one |
| DHT11 | VCC / GND | 3V3 / GND | |
| 16x2 I2C LCD | SDA | **GPIO 21** | ESP32 default I2C bus |
| 16x2 I2C LCD | SCL | **GPIO 22** | ESP32 default I2C bus |
| 16x2 I2C LCD | VCC / GND | 5V (VIN) / GND | Most LCD backpacks want 5 V for a readable display |
| Buzzer (active) | + | **GPIO 19** | Active buzzer = sounds when powered, no tone needed |
| Buzzer | - | GND | |
| RGB LED | R | **GPIO 25** via 220 ohm | |
| RGB LED | G | **GPIO 26** via 220 ohm | |
| RGB LED | B | **GPIO 27** via 220 ohm | |
| RGB LED | common | GND (common-cathode) | Common-anode LED: wire common to 3V3 and set `LED_COMMON_ANODE = true` |

Power: USB power bank into the ESP32's USB port (as in the prototype photo).

## Important: the RGB LED pins changed

The very first prototype sketch used GPIO **23 / 22 / 21** for the RGB LED. GPIO 21 and
22 are also the ESP32's default I2C pins (SDA/SCL) used by the LCD, so the LED and LCD
were on the same pins. If your prototype is wired that way, **move the three LED wires
to GPIO 25, 26 and 27** (or change `LED_R_PIN`, `LED_G_PIN`, `LED_B_PIN` to pins you
actually used - just never 21 or 22).

## Status outputs

| LED | Meaning |
|---|---|
| Red | Hazard active |
| Green | Normal |
| Blue | DHT sensor error |

At power-on the LED cycles red, green, blue and the buzzer chirps once. If one colour
is missing, that channel is miswired.

## Notes

- **LCD address:** `0x27` is the common default. If the backlight is on but the screen is
  blank or shows boxes, try `0x3F`, and turn the small blue contrast screw on the back.
  An I2C scanner sketch will tell you the real address.
- **Buzzer current:** a GPIO pin can safely drive a small active buzzer (about 20 mA). For
  anything bigger, switch it through an NPN transistor or MOSFET.
- **Antenna:** keep the GPS antenna facing the sky. Indoors it will usually show
  `GPS: No Signal`.
- **WROVER boards:** on ESP32-WROVER modules (the ones with PSRAM) GPIO 16 and 17 are
  used internally. If that is your board, pick two other free pins for the GPS and update
  `GPS_RX_PIN` / `GPS_TX_PIN`.
- **Strapping pins:** the pins used here avoid the ESP32's boot-strapping pins
  (0, 2, 5, 12, 15), so wiring will not stop the board from booting or flashing.
