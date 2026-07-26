# T-Deck Plus — Hardware Reference (authoritative)

Pin map and peripheral details for the LILYGO **T-Deck Plus** (ESP32-S3), taken
from LILYGO's official repo — **not guessed**. Use this file as ground truth when
writing `include/config.h` and drivers.

**Source:** `Xinyuan-LilyGO/T-Deck` — `examples/UnitTest/utilities.h` (pin macros)
and `examples/UnitTest/UnitTest.ino` (display/keyboard/GPS/trackball init).
Fetched 2026-07-25. Re-verify against that repo if anything doesn't match on
real hardware.

## Pin map (from `utilities.h`)

| Macro | GPIO | Notes |
|-------|------|-------|
| `BOARD_POWERON` | 10 | **Drive HIGH early in setup()** to power display/keyboard/radio/GPS |
| `BOARD_BOOT_PIN` | 0 | BOOT button; also used as **trackball click** |
| `BOARD_TFT_CS` | 12 | ST7789 chip-select |
| `BOARD_TFT_DC` | 11 | ST7789 data/command |
| `BOARD_TFT_BACKLIGHT` / `BOARD_BL_PIN` | 42 | Backlight, PWM, 16 brightness levels |
| `BOARD_SPI_SCK` | 40 | Shared SPI clock (display + LoRa + SD) |
| `BOARD_SPI_MOSI` | 41 | Shared SPI MOSI |
| `BOARD_SPI_MISO` | 38 | Shared SPI MISO |
| `BOARD_I2C_SDA` | 18 | Shared I2C (keyboard, touch) |
| `BOARD_I2C_SCL` | 8 | Shared I2C |
| `BOARD_TOUCH_INT` | 16 | Touch interrupt |
| `BOARD_KEYBOARD_INT` | 46 | Keyboard interrupt |
| `BOARD_TBOX_G01` | 3 | Trackball direction |
| `BOARD_TBOX_G02` | 2 | Trackball direction |
| `BOARD_TBOX_G03` | 15 | Trackball direction |
| `BOARD_TBOX_G04` | 1 | Trackball direction |
| `RADIO_CS_PIN` | 9 | SX1262 CS (LoRa — unused in M1) |
| `RADIO_BUSY_PIN` | 13 | SX1262 BUSY |
| `RADIO_RST_PIN` | 17 | SX1262 RST |
| `RADIO_DIO1_PIN` | 45 | SX1262 DIO1 |
| `BOARD_GPS_TX_PIN` | 43 | GPS UART (device TX ← module) |
| `BOARD_GPS_RX_PIN` | 44 | GPS UART (device RX → module) |
| `BOARD_SDCARD_CS` | 39 | microSD CS (unused in M1) |
| `BOARD_I2S_WS` | 5 | Speaker I2S word-select |
| `BOARD_I2S_BCK` | 7 | Speaker I2S bit-clock |
| `BOARD_I2S_DOUT` | 6 | Speaker I2S data out |
| `BOARD_ES7210_MCLK` | 48 | Mic codec (unused in M1) |
| `BOARD_ES7210_LRCK` | 21 | Mic codec |
| `BOARD_ES7210_SCK` | 47 | Mic codec |
| `BOARD_ES7210_DIN` | 14 | Mic codec |
| `BOARD_BAT_ADC` | 4 | Battery voltage ADC |

## Display

- Controller: **ST7789**, **320×240**.
- `tft.setRotation(1)` (landscape, matches the keyboard-down orientation).
- Backlight on `BOARD_BL_PIN` (GPIO42) via PWM — 16 brightness levels
  (`setBrightness(0..16)`).
- Startup: `digitalWrite(BOARD_POWERON, HIGH)` **before** touching the panel.
- Touch max coordinates set to 320×240.

## Keyboard

- LILYGO keyboard MCU on I2C address **`0x55`** (shared I2C bus, SDA 18 / SCL 8).
- Read one byte per poll: `Wire.requestFrom(0x55, 1); key = Wire.read();`.
- A returned byte of **`0` means no key**; any other byte is the ASCII/keycode.
- `BOARD_KEYBOARD_INT` (GPIO46) can signal key availability.

## GPS

- Module: L76K on UART, pins TX 43 / RX 44.
- Default baud **9600**, `SERIAL_8N1`. Firmware may retry 38400/115200 on failure.
- NMEA sentences (GGA/RMC) carry lat/lon/fix/sats/speed/date/time/altitude/HDOP.

## Trackball

- 4 direction lines: `BOARD_TBOX_G01..G04` (GPIO 3, 2, 15, 1).
- **Click/center = `BOARD_BOOT_PIN` (GPIO0)** (shared with the BOOT button).
- Map directions + click to LVGL `LV_KEY_UP/DOWN/LEFT/RIGHT/ENTER`.

## Battery

- ADC on `BOARD_BAT_ADC` (GPIO4); apply the board's resistor-divider ratio to get
  battery volts, then map to percent (≈3.3 V = 0 %, 4.2 V = 100 %).

## Gotchas

- **Everything is dark until `BOARD_POWERON` is HIGH** — no display, no I2C
  keyboard, no GPS.
- SPI bus is **shared** by display, LoRa, and SD — mind CS lines when LoRa is
  added in M3.
- I2C bus is **shared** by keyboard and touch.
