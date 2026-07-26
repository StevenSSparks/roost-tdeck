# RoostOS Communicator 🐤📡

A handheld **Claude AI communicator** for the **LILYGO T-Deck Plus** — chat with
Claude over WiFi on a pocket QWERTY device with a real screen. Part of the
[RoostOS](https://roostos.dev) family.

<!-- boot splash: RoostOS wordmark + a chick perched on an antenna -->

## What it does (today)
- On-screen chat with **Claude Haiku** over your WiFi (Anthropic Messages API).
- Branded **boot splash** (the RoostOS chick-on-antenna).
- Smooth, **adjustable fonts** (message + input independently) and **configurable
  colors**, an on-device **Settings** screen, and **scrollback**.
- Everything you change is **saved** (survives reboot).
- Physical QWERTY input; trackball to scroll + open settings. (Touch, maps, GPS,
  and games are on the roadmap below.)

## Hardware
**LILYGO T-Deck Plus** (ESP32-S3, 16 MB flash, 8 MB PSRAM, 2.8" 320×240 ST7789,
QWERTY keyboard, trackball, capacitive touch, GPS, LoRa). A USB-C cable to flash.

## Get set up (clone → configure → flash → fun)

### 1. Get the keys you need
- **Anthropic API key** (required, for chat): sign in at
  <https://console.anthropic.com> → **API Keys** → *Create Key*. Copy it
  (starts with `sk-ant-...`).
- **Geoapify key** (optional, for maps — roadmap): free at
  <https://myprojects.geoapify.com> → create a project → copy the API key.

### 2. Install the toolchain
Install **PlatformIO** (`brew install platformio`, or the VS Code extension).

### 3. Clone and set your values
```sh
git clone https://github.com/StevenSSparks/roost-tdeck.git
cd roost-tdeck
cp include/secrets.example.h include/secrets.h     # git-ignored; never committed
$EDITOR include/secrets.h                          # fill in your values
```
`include/secrets.h`:
```c
#define ANTHROPIC_API_KEY "sk-ant-...your key..."
#define GEOAPIFY_KEY      "...your geoapify key..."   // optional (maps)
#define DEFAULT_WIFI_SSID "YourWiFiName"              // 2.4 GHz network
#define DEFAULT_WIFI_PASS "YourWiFiPassword"
```
> Notes: the ESP32-S3 is **2.4 GHz only** — use a 2.4 GHz SSID. Secrets live only
> in `include/secrets.h`, which is git-ignored — they are never committed.
> (macOS owners can instead store these in the login keychain; `scripts/gen_secrets.py`
> pulls them at build time and won't overwrite a hand-written `secrets.h`.)

### 4. Flash it
```sh
pio run -e app -t upload      # build + flash the on-screen app
pio device monitor -e app     # (optional) watch the serial console
```

### 5. Use it
- **Type a message + Enter** on the keyboard to chat with Claude.
- **Open Settings:** press the **trackball center button** (touch coming soon).
  Roll the trackball to move, click to change, "Back to chat" to exit.
- **Scroll** the chat: roll the trackball up/down.
- **On-device commands** (type in the chat, abbreviations OK):
  `/font small|medium|large` · `/inputfont ...` · `/color user|ai <name>` ·
  `/scroll <n>` · `/splash <ms>` · `/settings` · `/set`, `/fon`, `/col` …

## Build environments
| Env | What |
|-----|------|
| `app`   | The on-screen firmware (this is the device). |
| `smoke` | A serial/TCP dev shell (WiFi/`ask`/`gps`/`bat`) — handy for bring-up. |
| `native`| Off-device unit tests: `pio test -e native`. |

## Roadmap
Touch (GT911) as primary input + tap Settings + swipe scroll · on-screen tool use
(Claude reads GPS / battery / renders **maps**) · GPS screen · WiFi/API editing +
more toggles in Settings · remote shell (telnet now, **SSH** later) · **games**
(Snake, Sudoku) · custom boot artwork · WiFi auto-reconnect. See `MORNING.md` and
`docs/superpowers/plans/` for the working plan.

## Safety
No secrets are ever committed. Stock firmware can be backed up before flashing
(`backups/`). This overwrites the LILYGO factory firmware — keep a backup if you
want to restore it.
