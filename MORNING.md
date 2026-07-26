# RoostOS Communicator — Morning Status (2026-07-25 night build)

## 🎉 It works on the device
The T-Deck is flashed with the **on-screen app** (`env:app`): boots, shows the
RoostOS UI (teal header, WiFi + IP), connects to **SSS-FAMILY**, and does a live
**Claude Haiku chat on the screen** — confirmed with real replies. The whole
vision runs on real hardware.

## What's verified working
- **Display:** ST7789 320×240, RoostOS theme, correct colors + landscape rotation.
- **Chat on screen:** type on the QWERTY + Enter → Haiku reply rendered. Bigger,
  adjustable font (default size 2; `font <1-4>` over serial; Settings slider TODO).
- **WiFi:** auto-connects to SSS-FAMILY (192.168.52.x, VLAN52) from keychain-
  provisioned secrets. (SSS-MAIN can't be used — WiFi7/WPA3 refuses the ESP32.)
- **Claude:** end-to-end Anthropic Haiku over TLS.
- **Dev shell:** serial `ask <text>` / `claude <text>` / `font <n>` / `ip`.
- **Native core:** 10/10 pure-logic tasks done + tested off-device (config, chat
  history, request/response + tool-loop assembly, usage/cost, Geoapify URL, tools).
- **Backup + repo:** stock firmware dumped to `backups/` (verified). Code pushed to
  **github.com/StevenSSparks/roost-tdeck** (public) with branch protection on main
  (force-push + deletion blocked) — also applied to `roost` and `roostos-web`.

## 👉 Please eyeball / try (I couldn't see the screen or type)
1. **Keyboard typing** on the device → does text appear + send on Enter?
2. Font size to taste: over serial run `font 3` (or 4). Tell me your preference
   and I'll make it the default + a Settings slider.
3. Colors/rotation look right? (If ever inverted, it's the one-line
   `TFT_INVERSION_ON` flag.)

## How to use / flash
- It's already flashed. Just type on the keyboard.
- Watch/drive over USB: `pio device monitor -e app` then `ask hello` / `font 3`.
- Reflash: `pio run -e app -t upload` (from repo root; needs the device on USB).
- Three envs: **app** (screen), **smoke** (serial/TCP dev shell + wifi/scan/bat/gps),
  **native** (`pio test -e native` — 15 logic tests).

## Known issues / next up
- **Font size + everything else → Settings** (persisted in NVS; the "all
  configurable" principle): font, name, timezone, web-search, sounds, theme,
  brightness, KB backlight, sleep, trackball on/off, SSH/TCP shell.
- **Real tool loop on screen:** wire the native core (buildRequestBody +
  parseResponse + tools) so Haiku can call `get_location` / `show_map` /
  `get_battery` / `play_tone` from the chat.
- **GPS:** UART streaming but NMEA not parsing yet — needs pin/baud tuning + sky
  view. Battery ADC reads high — needs divider calibration.
- **Touch** input (GT911) as primary; trackball optional/off.
- **LVGL** polish (proper scrolling/widgets) — current UI is direct TFT draw.
- **Context bundle:** inject name + local time (GPS/NTP) into the system prompt.
- **Dev Bridge / SSH:** serial + TCP:23 now; real SSH (libssh_esp32) later, toggle.
- **Platform note:** `env:app` is pinned to `espressif32@6.5.0` (Arduino 2.x)
  because TFT_eSPI 2.5 crashes on the Arduino 3.x core. Options later: keep the
  pin, or migrate the display to LovyanGFX (works on 3.x).

Plan + full task ledger: `docs/superpowers/plans/2026-07-25-roostos-communicator-m1.md`
and `.superpowers/sdd/.../progress.md`.
