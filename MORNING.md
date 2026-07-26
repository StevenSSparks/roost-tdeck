# RoostOS Communicator — Morning Status

## Session 3 (2026-07-26) — multi-provider + categorized Settings + config UX
**Done & on the device (env:app, verified over serial):**
- **Multi-provider AI:** Anthropic / OpenAI / Gemini / **local Ollama**. `askAI()`
  branches per provider; provider + model persist in NVS. Reply is labelled
  (Haiku/GPT/Gemini/Ollama). **Verify-then-switch:** `switchProvider()` only
  activates a provider whose key is configured, and **pings a local Ollama
  (`/api/tags`) before switching** — confirmed on hardware: Anthropic answers,
  Gemini transport/auth correct (owner's demo key is quota-exhausted → limit 0),
  Ollama switch **correctly refused** when `ai.senzall.net:11434` was unreachable.
- **Categorized Settings** (the BlackBerry ask): a main page → per-category
  sub-pages **Display / Colors / AI Provider / System-About**, each ≤6 items with
  item 0 = Back. AI Provider page lists all four with `ready`/`no key` + `*` on
  active. Trackball/touch/serial nav all use `pageLen(setPage)`. Nav verified.
- **Config builder (computer-side UX):** `tools/config/index.html` — a
  self-contained, offline page that generates `secrets.h` from a form (WiFi +
  keys + default provider). Nothing is uploaded. Also **hosted** at
  **roostos.dev/tdeck/config** (in the `roostos-web` repo).
- **roostos.dev/tdeck** landing page added to `roostos-web`.
- README updated (four providers, categorized Settings, config builder,
  `/provider` + `/model` commands).

**Still open (unchanged priorities):** GT911 touch calibration; on-screen tool
loop (GPS/battery/map); WiFi/API editing on device + more toggles; remote
shell/SSH; games; real chick PNG on splash; test Ollama once the box is reachable
from the device's VLAN (Ollama must listen on `0.0.0.0:11434`).

---

# (earlier) Morning Status (2026-07-25 night build)

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

---

## Update — richer chat UX (later that night)
Now on the device (env:app):
- **Branded boot splash** (RoostOS wordmark + drawn chick-on-antenna). Swap in real
  chick artwork later (hand me a PNG → I convert + embed).
- **Smooth fonts** (FreeSans), separate sizes for **messages** and the **input box**.
- **Configurable colors** for your messages and Haiku's (yellow prompt kept).
- **Scrollback**: roll the **trackball up/down** to scroll chat history (a "^ more"
  hint shows when scrolled up). After each send, your message stays visible at the
  top of the exchange.
- Plain-text replies (told Haiku no markdown/emoji for the pixel screen).

### Serial dev commands (`pio device monitor -e app`)
- `ask <text>` — chat (also type on the keyboard + Enter)
- `font <tiny|small|medium|large>` or `font <9|12|18>` — message size
- `inputfont <...>` — input-box size
- `color user <name>` / `color ai <name>` — names: teal indigo amber red green cyan magenta orange ink dim
- `ip`

### Please check in the morning
- **Keyboard typing** works? (no arrow keys on this pad — trackball is the scroller)
- **Trackball scroll** direction — I guessed pin3=up/pin2=down; tell me if reversed
  or if a different axis scrolls, and I'll remap.
- Your favorite **message/input font sizes + colors** → I'll make them the defaults
  and add the **Settings** screen to persist everything (NVS).

### More backlog (requested)
- **Games:** Snake (trackball/touch steer), Sudoku (QWERTY digits + cursor). Post-M1 fun.
- **Touch (GT911):** capacitive swipe up/down to scroll chat; becomes the primary
  input (trackball optional) — enables touch buttons + game controls. INT on GPIO16.
- Fun note: the device's own Haiku suggested "Tetris/Snake" unprompted when asked for
  fun uses — it's writing its own roadmap.

---

## Session 2 (late night) — Settings, persistence, more
**Added & verified (serial):** on-device **Settings screen** (open with trackball
**click**; roll=move, click=change, "Back to chat"); **About** screen (version,
device, MAC, IP, WiFi, heap/PSRAM, uptime, touch); **NVS persistence** (font,
colors, scroll rate, splash all survive reboot); smooth **FreeSans** fonts w/
small default; **configurable colors**; **scroll indicators** (▴/▾); on-device
**/commands** with short aliases (`/fon`,`/set`,`/col`,`/scr`,`/spl`); message
anchored so **your question shows at the top** of each reply. Community **README**
(get keys → clone → set `secrets.h` → flash). `gen_secrets.py` no longer clobbers a
hand-written `secrets.h` (distribution-friendly).

**Open Settings today:** press the **trackball center button**. (Menu ☰ is drawn
top-right for touch, but touch isn't calibrated yet.)

## TOMORROW — pick up here
1. **Touch (GT911):** my point-parsing returns garbage coords — fix the register
   read + calibrate the raw→screen mapping (rotation 1). Then: tap ☰ to open
   settings, tap rows to change, tap ▴/▾ + swipe to scroll. Make touch the primary
   input; trackball optional.
2. **Test the MAP features:** wire Geoapify `show_map` and render a map on-screen
   (needs the GEOAPIFY key). Add a GPS screen.
3. **On-screen tool loop:** let Claude call get_location / get_battery / show_map /
   play_tone from the chat (wire the native core into the app).
4. **Settings additions:** WiFi + API status/edit, **remote-shell on/off** toggle,
   trackball on/off, sounds, brightness, KB backlight, name+timezone. WiFi
   **auto-reconnect** (it failed to associate on one boot tonight).
5. **Remote shell / SSH:** add the telnet (TCP:23) shell to the app (behind the
   Settings toggle), then real SSH via `libssh_esp32` — like spiderverse's admin shell.
6. **Games:** Snake, then Sudoku (trackball/touch).
7. **Boot art:** swap the drawn chick for the real RoostOS chick PNG (keep as logo).
8. **Confirm on device:** keyboard shift/alt numbers+symbols; About screen contents;
   the top-anchor "your message shows" feel; default font size.

Envs: `app` (device) · `smoke` (serial/TCP dev shell) · `native` (`pio test -e native`).
