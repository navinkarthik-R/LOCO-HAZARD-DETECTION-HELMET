# Wiring

Board: **ESP32 DevKit** (30/38-pin, 3.3 V logic). Pin numbers are GPIO numbers and match
the constants at the top of
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
| **MQ-2 gas module** | **AO** | **GPIO 34**, **through a voltage divider** | See [MQ-2](#mq-2-gas-sensor) - never connect AO directly |
| MQ-2 gas module | VCC / GND | **5V (VIN)** / GND | The heater needs 5 V |
| MQ-2 gas module | DO | not connected | The firmware reads the analog output |
| **MAX30102** | SDA | **GPIO 21** | Shared I2C bus with the LCD (address `0x57`) |
| MAX30102 | SCL | **GPIO 22** | Shared I2C bus with the LCD |
| MAX30102 | VIN / GND | 3V3 / GND | INT pin not used |
| 16x2 I2C LCD | SDA | **GPIO 21** | ESP32 default I2C bus (address `0x27`) |
| 16x2 I2C LCD | SCL | **GPIO 22** | ESP32 default I2C bus |
| 16x2 I2C LCD | VCC / GND | 5V (VIN) / GND | See [I2C voltage](#i2c-voltage-lcd--max30102) |
| Buzzer (active) | + | **GPIO 19** | Active buzzer = sounds when powered, no tone needed |
| Buzzer | - | GND | |
| RGB LED | R | **GPIO 25** via 220 ohm | |
| RGB LED | G | **GPIO 26** via 220 ohm | |
| RGB LED | B | **GPIO 27** via 220 ohm | |
| RGB LED | common | GND (common-cathode) | Common-anode LED: wire common to 3V3 and set `LED_COMMON_ANODE = true` |

Power: USB power bank into the ESP32's USB port (as in the prototype photo).

## MQ-2 gas sensor

**The analog output (AO) must go through a voltage divider.** On a 5 V-powered module AO
can swing up to roughly 5 V, and an ESP32 pin tolerates 3.3 V.

```
MQ-2 AO ----[ 10 kohm ]----+---- ESP32 GPIO 34
                           |
                      [ 20 kohm ]
                           |
                          GND
```

That scales the signal to two thirds, so 5 V becomes about 3.3 V.

- **GPIO 34 is an ADC1 pin.** ADC2 pins stop working while Wi-Fi is on, so don't move the
  gas input to one of them. GPIO 34 is input-only, which is fine here.
- **Heater current:** the MQ-2 heater draws on the order of 150 mA from 5 V (check your
  module's datasheet). A power bank handles it; a weak USB cable or a PC port may not.
- **Warm-up:** the heater needs time to stabilise. The firmware ignores gas for the first
  **2 minutes** after power-on and uses the last 10 seconds of that period as the clean-air
  baseline - so **switch the helmet on in clean air**. A brand-new MQ-2 needs far longer
  (the datasheet suggests a long first burn-in) before readings settle.
- The ESP32's ADC is non-linear, which is another reason the alarm is relative to a baseline
  rather than an absolute value. See the README's calibration section.

## MAX30102 heart-rate / SpO2 sensor

- Connects to the **same I2C bus as the LCD** (GPIO 21/22). The two have different
  addresses (`0x57` and `0x27`), so they coexist fine.
- **Mounting matters more than anything else.** The LEDs and photodiode must press against
  skin with steady, light pressure and no ambient light leaking in. On a helmet that means
  a padded mount against the forehead or temple. Hair, sweat, movement and a loose fit all
  ruin the reading. For a first test, just press a fingertip on the sensor.
- If the firmware logs `MAX30102 not found`, check SDA/SCL, power (3.3 V) and the address.

## I2C voltage (LCD + MAX30102)

Most LCD backpacks run from 5 V **and pull the I2C lines up to 5 V**. With the MAX30102
and the ESP32 on the same lines, that puts 5 V on pins rated for 3.3 V. Many hobby builds
get away with it, but it is out of spec and can damage the sensor or the ESP32 over time.
Pick one:

1. Power the LCD backpack from **3V3** instead (the display is dimmer and some backpacks
   won't work at 3.3 V - try it), or
2. Use a bidirectional **I2C level shifter** between the ESP32/MAX30102 and the LCD, or
3. Remove the backpack's pull-up resistors (usually two small SMD resistors or solder
   jumpers - check your board) and rely on the 3.3 V pull-ups on the MAX30102 breakout.

## Important: the RGB LED pins changed

The very first prototype sketch used GPIO **23 / 22 / 21** for the RGB LED. GPIO 21 and
22 are also the ESP32's default I2C pins (SDA/SCL), used by the LCD and now the MAX30102
too, so the LED and the I2C bus were on the same pins. If your prototype is wired that way,
**move the three LED wires to GPIO 25, 26 and 27** (or change `LED_R_PIN`, `LED_G_PIN`,
`LED_B_PIN` to pins you actually used - just never 21 or 22).

## Status outputs

| LED | Meaning |
|---|---|
| Red | A hazard is active (heat/humidity, gas, or wearer vitals) |
| Green | Normal |
| Blue | DHT sensor error |

At power-on the LED cycles red, green, blue and the buzzer chirps once. If one colour is
missing, that channel is miswired.

## Notes

- **LCD address:** `0x27` is the common default. If the backlight is on but the screen is
  blank or shows boxes, try `0x3F`, and turn the small blue contrast screw on the back.
  An I2C scanner sketch will tell you the real address of every device on the bus.
- **Buzzer current:** a GPIO pin can safely drive a small active buzzer (about 20 mA). For
  anything bigger, switch it through an NPN transistor or MOSFET.
- **Antenna:** keep the GPS antenna facing the sky. Indoors it will usually show
  `GPS: No Signal`.
- **WROVER boards:** on ESP32-WROVER modules (the ones with PSRAM) GPIO 16 and 17 are
  used internally. If that is your board, pick two other free pins for the GPS and update
  `GPS_RX_PIN` / `GPS_TX_PIN`.
- **Strapping pins:** the pins used here avoid the ESP32's boot-strapping pins
  (0, 2, 5, 12, 15), so wiring will not stop the board from booting or flashing.
- **Not intrinsically safe:** this is a hobby circuit. It is not rated for use in
  explosive atmospheres, so do not rely on it where flammable gas could ignite.
