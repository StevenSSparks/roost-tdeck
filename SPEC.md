# T-Deck Claude Communicator — Build Spec

**For:** Claude Code, running on macOS.
**Deliverable:** firmware for a LILYGO T-Deck (Plus) that is a handheld chat
client — talks to Anthropic's Claude over WiFi **and** to other devices over the
LoRa packet network.

> **Repo placement (decide first):** Prefer a **standalone repo**
> `tdeck-claude-communicator` (PlatformIO project). If the owner keeps a personal
> monorepo (working name "roost"), place it under `roost/tdeck-claude-communicator/`
> instead. Do NOT put it in the website/arcade repo — different toolchain entirely.

---

## 0. Goal
A pocket "communicator" on the T-Deck's screen + keyboard that can:
1. Manage WiFi (scan, pick, enter password, connect, remember networks).
2. Detect captive portals and get onto hotel/airport WiFi via a MAC-clone workaround.
3. Chat with **Claude Haiku** over the internet (Anthropic Messages API).
4. Send/receive text with **other LoRa devices** (e.g., a XIAO ESP32-S3 + SX1262).
5. **Bridge mode:** answer a LoRa query by asking Claude over WiFi and radioing the reply back.

---

## 1. Hardware target
**LILYGO T-Deck / T-Deck Plus**
- MCU: **ESP32-S3** (16 MB flash, 8 MB PSRAM). Enable PSRAM (OPI).
- Display: **2.8" 320×240 ST7789**, SPI.
- Keyboard: physical QWERTY, read over **I2C** (a co-processor at address **0x55**; read 1 byte = keycode).
- Trackball: 4 direction GPIOs + click (used for navigation).
- Radio: **Semtech SX1262 LoRa** (915 MHz US), on SPI — drive with **RadioLib**.
- GPS: u-blox (T-Deck Plus only), on UART.
- Also: I2S mic/speaker, microSD slot, LiPo + charging.
- **Critical:** a **board-power-enable pin (GPIO 10)** must be driven HIGH early in
  setup() to power the peripherals (display/keyboard/radio). Verify exact pins
  against LILYGO's official T-Deck repo before wiring drivers.

> Pin map + working peripheral init: use LILYGO's official `T-Deck` GitHub repo and
> the `RadioLib` + `TFT_eSPI`/`LVGL` examples as ground truth. Don't guess pins.

---

## 2. Toolchain & libraries
- **PlatformIO** (preferred) with the Arduino-ESP32 framework. Board: an ESP32-S3
  config with PSRAM enabled, 16 MB flash. Provide a working `platformio.ini`.
- UI: **LVGL** (nicer) or **TFT_eSPI** (simpler). Pick one; LVGL recommended for the
  list/chat views.
- Radio: **RadioLib** (SX1262).
- HTTP/TLS: `WiFiClientSecure` + `HTTPClient` (built in).
- JSON: **ArduinoJson**.
- Persistence: **Preferences/NVS** (built in).

---

## 3. Features

### 3.1 WiFi Manager
- Scan (`WiFi.scanNetworks()`), show a scrollable list with SSID + RSSI + lock icon.
- Select with trackball/keys; type password on the keyboard; connect with a status
  spinner and clear success/fail feedback.
- **Saved networks** in NVS; auto-reconnect to known SSIDs on boot.
- Manual "forget network" and "rescan."

### 3.2 Captive portal detection + workaround
- **Detection:** after associating, GET a known check URL (e.g.
  `http://connectivitycheck.gstatic.com/generate_204` or `http://captive.apple.com`).
  Expected = 204 / known body. If redirected or unexpected → **captive portal present**.
- **MAC-clone workaround (primary):** allow the user to enter/save a MAC address and
  apply it with `esp_wifi_set_mac(WIFI_IF_STA, mac)` **before** connecting, so the
  device inherits a session already authenticated on that MAC (log in once on a phone,
  clone here). Store recent MACs. Show a short explainer in-app.
- **Simple auto-accept (best-effort):** if the portal is a trivial no-JS "click accept"
  form, optionally fetch it, find the form action, and POST it. Wrap in try/……; never
  block on it.
- **Non-goal:** rendering/clicking arbitrary hotel portal pages — impossible on an
  ESP32 (no browser). Detect + MAC-clone is the supported path. Say so in the UI.

### 3.3 Claude chat (Anthropic Messages API)
- `POST https://api.anthropic.com/v1/messages` over TLS.
- Headers: `x-api-key: <key>`, `anthropic-version: 2023-06-01`, `content-type: application/json`.
- Body: `{ "model": <configurable>, "max_tokens": <configurable, default 512>,
  "system": <optional persona>, "messages": [ {role, content}, ... ] }`.
- Keep a rolling **conversation history** (trim oldest to stay within a token budget).
- Parse `content[0].text` from the JSON response (ArduinoJson) and render it.
- **Model is configurable** in settings; default to the current Haiku model ID from
  docs.claude.com/en/docs/about-claude/models (do NOT hard-code a stale ID).
- **Streaming (SSE)** = stretch goal; ship non-streaming first (simpler, robust).
- Handle errors (no internet, 401 bad key, 429 rate limit, timeouts) with readable messages.

### 3.4 LoRa packet messaging (device-to-device)
- RadioLib SX1262, **915 MHz** (US ISM). Fixed, documented radio params (frequency,
  spreading factor, bandwidth, coding rate, sync word) — the SAME on all devices or
  they can't hear each other. Put them in one config header.
- Send/receive short text messages to/from other SX1262 devices (the XIAO node, another T-Deck).
- **WiFi + LoRa run concurrently** on the ESP32-S3 (WiFi = built-in radio, LoRa = SPI
  chip) — the app uses both at once. Service the radio via interrupt/`available()` in loop.
- Reliability: sequence numbers + optional ACK + retransmit. De-dupe by (src,seq).
- Encryption: AES-128 on the payload with a shared key from settings (LoRa is not
  private by default).

### 3.5 Bridge mode
- When enabled and WiFi is up: a LoRa message of type `ASK` → forward text to Claude →
  send Claude's reply back over LoRa as type `REPLY` to the requester's device ID.
- Chunk replies across multiple LoRa packets if longer than one payload; reassemble on
  the receiver. Keep replies short (set a low `max_tokens` in bridge mode).
- **Abuse guards (required for a public gateway):** cap `max_tokens`, throttle requests
  per sender device ID, and allowlist known device IDs — otherwise every inbound LoRa
  message spends Anthropic credits. Log spend if possible.
- The gateway's Claude call target is **configurable**: Anthropic directly, or the Pi
  proxy (see §6.4) so the key stays off the radio node.

### 3.6 Settings & persistence (NVS)
- Anthropic API key, model, max_tokens, system persona.
- LoRa: this device's ID, params, AES key, bridge on/off.
- Saved WiFi networks + saved MACs to clone.
- First-run onboarding screen to enter the API key (and a way to update it later).

---

## 4. UI / screens
- **Boot** → power-enable peripherals, init display/keyboard/radio, auto-connect WiFi.
- **Main menu:** `Chat with Claude` · `Mesh Chat` · `WiFi` · `Settings`.
- **WiFi screen:** network list, password entry, status, saved nets, captive-portal
  banner + "Clone MAC" action.
- **Chat screen:** scrolling history (user vs Claude styled), input line, send.
- **Mesh screen:** list of received LoRa messages + compose/send; pick destination ID.
- **Settings screen:** API key/model, LoRa params/ID/AES key, bridge toggle.

---

## 5. LoRa packet format (custom, documented)
Binary, little-endian, ≤ ~200-byte payload. Suggested header:
```
uint8  version        // protocol version (start at 1)
uint8  type           // 0=CHAT 1=ASK 2=REPLY 3=ACK
uint8  src_id         // sending device id
uint8  dst_id         // 0xFF = broadcast
uint16 msg_id         // sequence, for ACK/de-dupe
uint8  frag_idx       // fragment index (for chunked replies)
uint8  frag_total     // total fragments
// ...then AES-128 encrypted payload bytes...
```
Document this in `PROTOCOL.md` so other devices (the XIAO node) implement the same format.

---

## 6. Secrets & API-key provisioning
**Do NOT bake the Anthropic key into the committed firmware.** Support, in priority order:

1. **NVS provisioning (default).** Store the key in NVS (Preferences), entered at runtime:
   - **T-Deck:** typed on the on-device keyboard in a first-run/setup screen (and editable in Settings).
   - **Headless nodes (XIAO gateway):** a WiFi setup AP + web form, or pasted over USB serial.
   The compiled binary contains no secret; the key survives reboots; changing it needs no reflash.
   Enable **flash encryption** when practical so NVS isn't trivially dumpable.
2. **Dev-only compile-time key** behind a build flag (`#ifdef DEV_SECRETS`) reading a
   **gitignored** `secrets.h`. Ship `secrets.example.h`. Off by default; never commit `secrets.h`.
3. **SD-card config** (`config.json`) read at boot — convenient, but plaintext on removable media.
4. **Proxy mode (recommended for any public/deployed gateway).** The device does NOT hold the
   Anthropic key. It calls a **local endpoint (the Pi brain)** with a low-value shared token; the
   Pi holds the real key and forwards to Anthropic. Best if the gateway could be lost/stolen or is
   public-facing. Make the Claude call target **configurable** (Anthropic directly *or* a proxy URL)
   so the same firmware supports both.

**Notes:** ESP32 flash is readable without flash-encryption/secure-boot — assume a baked-in key can
be extracted with physical access. Keep all secrets out of git (`.gitignore` `secrets.h`, SD configs).
MAC-cloning uses the user's own authenticated session (the phone should leave the network so the MAC
isn't duplicated live).

---

## 7. Non-goals / honest constraints
- No captive-portal *browser* (ESP32 can't render hotel login pages) — detection +
  MAC-clone only.
- LoRa = small messages (hundreds of bytes), seconds of latency, ISM duty-cycle limits.
  Text/commands only, never images or web pages.
- Custom LoRa packets are **not** Meshtastic-interoperable — all peer devices must run
  this same protocol. (If Meshtastic interop is wanted later, that's a separate mode.)

---

## 8. Build & flash
- Provide `platformio.ini` (ESP32-S3, PSRAM OPI, 16 MB flash, monitor 115200).
- `pio run -t upload && pio device monitor`.
- README: how to set the API key on first boot, how to set matching LoRa params across
  devices, and how to pair with the XIAO node.

---

## 9. Milestones (implement in this order, commit each)
1. **M1 — Skeleton + WiFi + Chat:** boot/peripherals, WiFi manager, Claude chat over
   HTTPS, settings/NVS, on-screen chat UI. (Most of the value.)
2. **M2 — Captive handling:** portal detection + MAC-clone + simple auto-accept.
3. **M3 — LoRa messaging:** RadioLib SX1262, packet protocol, mesh chat screen, AES,
   ACK/retransmit, de-dupe.
4. **M4 — Bridge mode:** LoRa ASK → Claude → REPLY, with fragmentation.
5. **M5 — Polish:** error states, battery indicator, GPS (optional), saved-network UX.

Show a diff per milestone; keep `secrets.*` out of git; write the README + PROTOCOL.md.

---

## 10. Repo structure
```
tdeck-claude-communicator/
├── platformio.ini
├── src/            main.cpp + modules (wifi, portal, claude, lora, ui, settings)
├── include/        config.h, protocol structs
├── secrets.example.h
├── README.md
├── PROTOCOL.md     the LoRa packet format (shared with the XIAO node)
└── .gitignore      (secrets.h, .pio, build)
```

---

## Appendix — Anthropic Messages API (reference)
- Docs: https://docs.claude.com/en/docs/build-with-claude — Messages API + current model IDs.
- Minimal request body:
```json
{ "model": "<current-haiku-id>", "max_tokens": 512,
  "messages": [ { "role": "user", "content": "Hello!" } ] }
```
- Response text at `content[0].text`. Include `anthropic-version: 2023-06-01`.
