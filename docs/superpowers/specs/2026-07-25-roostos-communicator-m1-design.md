# RoostOS Communicator — M1 Design

**Date:** 2026-07-25
**Status:** Approved for planning
**Repo:** `StevenSSparks/roost-tdeck` (public) — local: `~/dev/tdeck-claude-communicator`
**Master spec:** `SPEC.md` (this document scopes an **expanded Milestone 1**)

---

## 1. Goal

A branded **RoostOS** handheld "communicator" on the LILYGO T-Deck Plus that:
boots the hardware, gets onto **any** WiFi (including captive-portal hotel/airport
networks), and chats with **Claude** over the Anthropic Messages API — where Claude
can **use the device itself** as tools: read GPS, render a map of the user's
location inline in the chat, report battery, and play tones. All runtime config
(API keys, model, persona, saved networks/MACs) lives on-device in NVS.

This is an **expanded M1** relative to `SPEC.md`: it folds in captive-portal
handling (SPEC M2) and adds on-device **tool use** and map rendering, which the
original spec did not contemplate. LoRa mesh (M3) and bridge mode (M4) remain
out of scope and get their own specs.

## 2. Hardware target

**LILYGO T-Deck Plus** — ESP32-S3, 16 MB flash, 8 MB PSRAM (OPI), internal
antenna. Display: 2.8" 320×240 ST7789 (SPI). Keyboard: physical QWERTY via I2C
co-processor at `0x55` (read 1 byte = keycode). Trackball: 4 direction GPIOs +
click. **Used in M1:** u-blox **GPS** (UART), **I2S speaker** (tones), **battery**
sense (ADC). Not used in M1: SX1262 LoRa, microSD, mic.

**Critical:** GPIO 10 (board power-enable) must be driven HIGH early in `setup()`
to power peripherals. Exact pins are verified against LILYGO's official T-Deck
repo before wiring drivers — pins are not guessed.

## 3. Toolchain

- **PlatformIO** + Arduino-ESP32. Board: ESP32-S3, PSRAM OPI, 16 MB flash,
  monitor 115200. Working `platformio.ini` provided.
- **UI: LVGL** — branded scrolling chat, list views, and inline **image bubbles**
  for maps. Keyboard + trackball wired as LVGL input devices.
- **HTTP/TLS:** `WiFiClientSecure` + `HTTPClient` (built in).
- **JSON:** ArduinoJson.
- **JPEG decode:** TJpg_Decoder (light ESP32 JPEG decoder) for map images ->
  LVGL image buffer.
- **Persistence:** Preferences/NVS.

## 4. Architecture (modules)

```
platformio.ini            ESP32-S3, PSRAM OPI, 16MB, monitor 115200; native test env
include/
  config.h                pins, defaults (model, max_tokens), brand colors, endpoints
  version.h               RoostOS Comm version string
  secrets.example.h       template; real secrets.h is gitignored (see §11)
src/
  main.cpp                boot sequence + LVGL tick loop + Claude tool loop pump
  board/board.cpp         power-enable (GPIO10), display, keyboard(I2C), trackball
  drivers/gps.cpp         u-blox UART parse -> {lat, lon, fix, sats}
  drivers/audio.cpp       I2S tone/beep generator (play_tone)
  drivers/battery.cpp     ADC -> voltage + charge %
  ui/theme.cpp            RoostOS LVGL theme (teal/indigo/amber on #0d1117)
  ui/ui.cpp               screen manager + navigation
  ui/screen_boot.cpp      splash -> status
  ui/screen_menu.cpp      Chat . WiFi . Stats . Settings
  ui/screen_wifi.cpp      scan list, password entry, saved nets, captive banner, Clone MAC
  ui/screen_chat.cpp      scrolling history, text + image bubbles, input line, Clear
  ui/screen_settings.cpp  API keys, model, max_tokens, persona, saved MACs, sound on/off
  ui/screen_stats.cpp     token usage + device/network details
  net/wifi_manager.cpp    scan / connect / NVS saved nets / auto-reconnect
  net/captive_portal.cpp  detection (204 check) + MAC-clone apply + saved MACs
  net/claude_client.cpp   Messages API + agentic tool loop, rolling history
  net/map_client.cpp      Geoapify static-map fetch + JPEG decode -> LVGL image
  tools/device_tools.cpp  tool registry + dispatch (get_location/show_map/get_battery/play_tone)
  config/settings.cpp     NVS (Preferences) wrapper — all persisted config
```

## 5. RoostOS brand theme

Matches `roostos.dev`:
- Backgrounds `#0d1117`/`#0a0e15`, panels `#151d2c`/`#1b2436`, edges `#243049`
- Text `#eef2fb` (ink), `#93a0c4` (dim)
- Accents `#34e2c0` (teal, primary), `#7c8cff` (indigo), `#ffbe4d` (amber);
  gradient teal -> indigo

## 6. Boot flow

1. Drive GPIO 10 HIGH.
2. Init display, keyboard (I2C 0x55), trackball, GPS (UART), audio (I2S), battery (ADC).
3. Init LVGL + apply RoostOS theme.
4. Load settings from NVS.
5. **First run?** If `DEV_SECRETS` seeded a key it's already in NVS; otherwise
   show onboarding to type the key.
6. Auto-connect a saved WiFi network; run captive-portal check.
7. Show main menu.

## 7. Chat + tool-use flow (agentic loop)

The device runs a bounded **tool loop** against the Messages API:

1. User types a message -> append to rolling history.
2. `POST https://api.anthropic.com/v1/messages` with headers
   `x-api-key: <NVS key>`, `anthropic-version: 2023-06-01`,
   `content-type: application/json`; body includes `model`, `max_tokens`,
   optional `system` persona, the `messages` history, and the `tools` array.
3. Parse the response with ArduinoJson:
   - `stop_reason: "end_turn"` -> render assistant text, done.
   - `stop_reason: "tool_use"` -> for each `tool_use` block, dispatch to
     `device_tools`, collect `tool_result` blocks, append assistant turn +
     user `tool_result` turn, and **loop** (capped at N iterations to bound
     cost/time).
4. History trimmed oldest-first to a token budget.

**Default model:** `claude-haiku-4-5` (configurable). **Default max_tokens:** 512.
Non-streaming. Tool-loop iteration cap is configurable (default small, e.g. 4).

### 7.1 Tools exposed to Claude

| Tool | Input | Action | tool_result |
|------|-------|--------|-------------|
| `get_location` | none | Read GPS fix | `{lat, lon, fix, sats}` or "no fix yet" |
| `show_map` | optional `lat`,`lon`,`zoom` | Fetch Geoapify static map (defaults to current GPS), render inline image bubble | short text confirming display + coords |
| `get_battery` | none | Read battery | `{volts, percent, charging}` |
| `play_tone` | `freq_hz`,`ms` | Emit a tone via I2S | "played" |

`show_map` performs the fetch+decode+display as a side effect and returns a text
summary to Claude (Claude does not receive the image bytes; the device shows it).

## 8. Map rendering (Geoapify)

`map_client` builds a Geoapify Static Maps URL centered on the location at a
**screen-legible scale** (image ~240×176, marker at center, zoom default ~15,
requested as **JPEG** for cheap decode). Fetch over TLS, decode with
TJpg_Decoder into a PSRAM buffer, and display as an LVGL image bubble in the chat.
Geoapify API key is stored in NVS (provisioned like the Anthropic key). Failures
(no key, no fix, HTTP/timeout, decode error) render a readable inline error
message, never a crash. **Privacy note in-app:** using the map sends your
coordinates to Geoapify.

## 9. WiFi + captive portal (any WiFi)

- **Manager:** scan (SSID/RSSI/lock), select, type password, connect with status;
  saved networks in NVS with auto-reconnect; forget/rescan.
- **Captive detection:** after associating, GET a known 204 check URL
  (e.g. `connectivitycheck.gstatic.com/generate_204`); a redirect/unexpected body
  raises a **captive-portal banner**.
- **MAC-clone workaround:** user enters/saves a MAC (recent MACs kept in NVS);
  apply with `esp_wifi_set_mac(WIFI_IF_STA, mac)` **before** connecting so the
  device inherits a session already authenticated on that MAC (log in once on a
  phone, clone here). Short in-app explainer. Auto-accept of trivial portals is
  **not** in this milestone.

## 10. Chat features

- **Clear chat** — button on the chat screen (and resets rolling history +
  clears image bubbles). Confirm before clearing.
- Scrollable multi-turn history with user/assistant styling and inline image
  bubbles.
- Persona/system prompt configurable in Settings.
- (Persisting chat history across reboots is **out of scope**; history is
  in-memory for the session.)

## 10a. Sounds

A persisted **Sounds on/off** toggle in Settings. When off, `play_tone` and all
UI beeps are muted (the tool still returns a clean `tool_result` so Claude isn't
confused). Default: on.

## 10b. Stats screen

A read-only Stats screen (menu entry) showing:
- **Tokens:** cumulative `input_tokens` / `output_tokens` this session (summed
  from each Messages API response `usage`), plus last-request tokens and an
  approximate cost estimate for the selected model.
- **Network:** WiFi SSID, RSSI, IP, station MAC, captive state.
- **Device:** GPS fix + sats, battery volts/percent, uptime, free heap, free
  PSRAM, firmware version.

Token counters reset with Clear chat and on reboot (session-scoped).

## 10c. Themes

A **theme picker** in Settings selects among a few LVGL palettes, persisted in
NVS and applied live:
- **RoostOS** (default) — teal/indigo/amber on `#0d1117`.
- **Roost Light** — light background variant of the brand palette.
- **Terminal** — amber-on-black monospace feel.
- **Spider-Verse** — nod to the sibling project (magenta/cyan on near-black).

Themes are data (palette structs) consumed by `ui/theme.cpp`; adding one is a
table entry, not new screens.

## 11. Config, secrets & dev provisioning (NVS)

All runtime config in NVS: Anthropic API key, **Geoapify key**, model,
max_tokens, persona, saved WiFi networks, saved MACs, tool-loop cap, **theme**,
**sounds on/off**, screen brightness, sleep timeout.

**Flash-time key provisioning (the "don't type it" ask) — from macOS Keychain:**
- The Anthropic and Geoapify keys live in the owner's **macOS login keychain**
  (items `anthropic_api_key_sparkshost` and `gtoapify_key_sfehost`).
- A PlatformIO **pre-build script** (`scripts/gen_secrets.py`, `extra_scripts`)
  runs `security find-generic-password -s <item> -w` to read each key and
  generates the **gitignored `include/secrets.h`**. (First run may prompt the
  Keychain to allow access.)
- Behind a `-DDEV_SECRETS` build flag, `secrets.h` seeds the keys into NVS on
  first boot if NVS is empty. Keys remain editable on-device and survive reflash.
- `secrets.example.h` is committed as a template; if the keychain lookup fails
  the build errors with a clear message rather than embedding blanks.
- The committed binary contains no secret unless built with `DEV_SECRETS` (owner
  device only). Physical-flash extraction remains an accepted limitation (flash
  encryption is later hardening).

`.gitignore` covers `.pio/`, `build/`, `secrets.h`, `*.bin`.

## 11a. Handheld niceties

- **Screen brightness** control and an inactivity **sleep/dim timeout**
  (backlight), both persisted.
- These are small extras included with themes; more can be added on request.

## 12. Error handling

- **WiFi:** wrong-password, no-internet, captive-portal banner + Clone MAC action.
- **Claude:** 401 (bad key), 429 (rate limit), timeout, no-connection, and
  tool-loop-cap-reached — all readable inline; never crashes.
- **Tools:** GPS no-fix, map fetch/decode failure, battery read error — each
  returns a clean `tool_result` so Claude can respond gracefully.

## 13. Testing

- **Off-device (PlatformIO `native` env):** settings serialization round-trips;
  rolling-history trimming; Claude request builder + response parser incl.
  `tool_use` extraction and `tool_result` assembly (canned JSON); Geoapify URL
  builder.
- **On-device:** flash to the T-Deck Plus and verify boot, WiFi connect (incl. a
  captive network via MAC-clone), a chat round-trip, and a
  `get_location -> show_map` sequence rendering a map bubble.

## 14. Out of scope (later milestones / specs)

- M3: LoRa SX1262 mesh chat, AES, ACK/retransmit, `PROTOCOL.md`.
- M4: bridge mode (LoRa ASK -> Claude -> REPLY, fragmentation).
- Auto-accept of trivial captive portals; SSE streaming; TTS/mic; persistent
  chat history; flash encryption.
