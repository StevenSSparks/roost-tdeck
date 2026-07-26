# RoostOS Communicator — M1 Design

**Date:** 2026-07-25
**Status:** Approved for planning
**Repo:** `StevenSSparks/roost-tdeck` (public) — local: `~/dev/tdeck-claude-communicator`
**Master spec:** `SPEC.md` (this document scopes **Milestone 1** of that spec)

---

## 1. Goal

A branded **RoostOS** handheld "communicator" on the LILYGO T-Deck Plus: boot the
hardware, manage WiFi, and chat with **Claude** over the Anthropic Messages API,
with all runtime config (API key, model, persona) stored on-device. This is
Milestone 1 (M1) of `SPEC.md` — "Skeleton + WiFi + Chat", described there as
"most of the value." Captive-portal handling (M2), LoRa mesh (M3), and bridge
mode (M4) are **out of scope** for this design and get their own specs.

## 2. Hardware target

**LILYGO T-Deck Plus** — ESP32-S3, 16 MB flash, 8 MB PSRAM (OPI), internal
antenna. Display: 2.8" 320×240 ST7789 (SPI). Keyboard: physical QWERTY via I2C
co-processor at address `0x55` (read 1 byte = keycode). Trackball: 4 direction
GPIOs + click. Also present but **not used in M1**: SX1262 LoRa, u-blox GPS, I2S
audio, microSD.

**Critical:** GPIO 10 (board power-enable) must be driven HIGH early in `setup()`
to power the peripherals (display/keyboard/radio). Exact pins are verified
against LILYGO's official T-Deck repo before wiring drivers — pins are not guessed.

## 3. Toolchain

- **PlatformIO** + Arduino-ESP32 framework. Board: ESP32-S3, PSRAM OPI enabled,
  16 MB flash, monitor 115200. A working `platformio.ini` is provided.
- **UI: LVGL** — chosen for branded scrolling list/chat views. Keyboard and
  trackball are wired as LVGL input devices.
- **HTTP/TLS:** `WiFiClientSecure` + `HTTPClient` (built in).
- **JSON:** ArduinoJson.
- **Persistence:** Preferences/NVS (built in).

## 4. Architecture (modules)

Each module has one clear purpose, a small interface, and is independently
reasoned about. UI screens are separate translation units so no single file
grows unwieldy.

```
platformio.ini            ESP32-S3, PSRAM OPI, 16MB, monitor 115200
include/
  config.h                pins, defaults (model, max_tokens), RoostOS brand colors
  version.h               RoostOS Comm version string
src/
  main.cpp                boot sequence + LVGL tick loop
  board/board.cpp         power-enable (GPIO10), display, keyboard(I2C), trackball init
  ui/theme.cpp            RoostOS LVGL theme (teal/indigo/amber on #0d1117)
  ui/ui.cpp               screen manager + navigation (trackball/keys)
  ui/screen_boot.cpp      splash -> status
  ui/screen_menu.cpp      Chat . WiFi . Settings
  ui/screen_wifi.cpp      scan list, password entry, status, saved nets
  ui/screen_chat.cpp      scrolling history + input line
  ui/screen_settings.cpp  API key, model, max_tokens, persona
  net/wifi_manager.cpp    scan / connect / NVS saved nets / auto-reconnect
  net/claude_client.cpp   Anthropic Messages API, rolling history
  config/settings.cpp     NVS (Preferences) wrapper — all persisted config
```

## 5. RoostOS brand theme

Matches `roostos.dev` (from `roostos-web`):

- Backgrounds: `#0d1117` (bg), `#0a0e15` (bg2), `#151d2c` / `#1b2436` (panels),
  `#243049` (edges)
- Text: `#eef2fb` (ink), `#93a0c4` (dim)
- Accents: `#34e2c0` (teal, primary), `#7c8cff` (indigo), `#ffbe4d` (amber)
- Gradient: teal -> indigo

The LVGL theme applies these globally so all screens read as a RoostOS device.

## 6. Boot flow

1. Drive GPIO 10 HIGH (power-enable peripherals).
2. Init display, keyboard (I2C 0x55), trackball.
3. Init LVGL + apply RoostOS theme.
4. Load settings from NVS.
5. **First run?** (no API key stored) -> onboarding screen to type the key.
6. Auto-connect to a saved WiFi network if any.
7. Show main menu.

## 7. Data flow — chat

Keyboard input -> input line -> append user turn to rolling history ->
`POST https://api.anthropic.com/v1/messages` over TLS with headers
`x-api-key: <NVS key>`, `anthropic-version: 2023-06-01`,
`content-type: application/json` -> body
`{ model, max_tokens, system?, messages: [...] }` -> parse `content[0].text`
with ArduinoJson -> render assistant bubble. History is trimmed oldest-first to
stay within a token budget.

**Default model:** `claude-haiku-4-5` (current Haiku; configurable in Settings).
**Default max_tokens:** 512 (configurable). Non-streaming (SSE streaming is a
later stretch goal per SPEC).

## 8. Config & secrets (NVS)

All runtime config lives in NVS (Preferences) — the compiled binary contains no
secret:

- Anthropic API key, model, max_tokens, system persona.
- Saved WiFi networks (SSID + password).

The API key is typed on the device at first boot and editable in Settings.
`.gitignore` covers `.pio/`, `secrets.h`, build artifacts. No key is ever
committed. (Physical-access flash extraction is an accepted limitation per SPEC;
flash encryption is a later hardening step.)

## 9. Error handling

- **WiFi:** clear feedback for wrong password, no-internet, and a
  captive-portal-suspected banner (detection only in M1; the MAC-clone
  workaround is M2). Never blocks the UI.
- **Claude:** readable inline messages for 401 (bad key), 429 (rate limit),
  timeout, and no-connection. Never crashes the device.

## 10. Testing

- **Off-device (PlatformIO `native` env):** pure logic unit tests — settings
  serialization round-trips, rolling-history trimming, and Claude request/
  response JSON shaping (build the request body, parse a canned response).
- **On-device:** flash to the T-Deck Plus and verify boot, WiFi connect, and a
  real chat round-trip. Hardware/LVGL behavior is validated on hardware.

## 11. Out of scope (later milestones / specs)

- M2: captive-portal MAC-clone workaround + auto-accept.
- M3: LoRa SX1262 mesh chat, AES, ACK/retransmit, `PROTOCOL.md`.
- M4: bridge mode (LoRa ASK -> Claude -> REPLY, fragmentation).
- SSE streaming, GPS, battery indicator, audio.
