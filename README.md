# RoostOS Communicator 🐤📡

A handheld **Claude AI communicator** for the **LILYGO T-Deck Plus** — chat with
Claude over WiFi on a pocket QWERTY device with a real screen. Part of the
[RoostOS](https://roostos.dev) family.

<!-- boot splash: RoostOS wordmark + a chick perched on an antenna -->

> 🌐 **Prefer clicking to typing?** Everything below — plus a browser-based
> `secrets.h` builder — is at **[roostos.dev/tdeck](https://roostos.dev/tdeck)**.

## What it does (today)
- On-screen chat over your WiFi with any of **four AI providers** — **Anthropic
  (Claude)**, **OpenAI (GPT)**, **Google Gemini**, or a **local Ollama** box.
  Pick the active one in Settings; only providers you've given a key (or a
  reachable Ollama host) can be selected, and switching **verifies the provider
  answers before activating**.
- Branded **boot splash** (the RoostOS chick-on-antenna).
- A **categorized Settings menu** (BlackBerry-style): a main page with
  per-category sub-pages — **Display**, **Colors**, **AI Provider**, **System /
  About** — each with a Back item.
- Smooth, **adjustable fonts** (message + input independently) and **configurable
  colors**, and **scrollback**.
- Everything you change is **saved** (survives reboot).
- Physical QWERTY input; trackball to scroll + navigate settings. (Touch, maps,
  GPS, and games are on the roadmap below.)

## Hardware
**LILYGO T-Deck Plus** (ESP32-S3, 16 MB flash, 8 MB PSRAM, 2.8" 320×240 ST7789,
QWERTY keyboard, trackball, capacitive touch, GPS, LoRa). A USB-C cable to flash.

## Get set up (clone → configure → flash → fun)

### 1. Get a key for at least one provider
You only need **one** to start — pick whichever you have:
- **Anthropic (Claude)** — <https://console.anthropic.com> → **API Keys** →
  *Create Key* (starts `sk-ant-...`). Recommended default.
- **OpenAI (GPT)** — <https://platform.openai.com/api-keys> (starts `sk-...`).
- **Google Gemini** — <https://aistudio.google.com/app/apikey> (free tier).
- **Ollama (local)** — no key; just the `host:port` of your box on the LAN
  (e.g. `192.168.1.50:11434`). Must be reachable from the device's network.
- **Geoapify key** (optional, maps — roadmap): <https://myprojects.geoapify.com>.

### 2. Install the toolchain
Install **PlatformIO** (`brew install platformio`, or the VS Code extension).

### 3. Clone and set your values

**Easiest — the config builder:** open
**[roostos.dev/tdeck/config](https://roostos.dev/tdeck/config)** (or the local
copy at `tools/config/index.html`), fill in your WiFi + key(s), and download a
ready-made `secrets.h`. It runs entirely in your browser — nothing is uploaded.

**Or by hand:**
```sh
git clone https://github.com/StevenSSparks/roost-tdeck.git
cd roost-tdeck
cp include/secrets.example.h include/secrets.h     # git-ignored; never committed
$EDITOR include/secrets.h                          # fill in your values
```
`include/secrets.h` (set the provider(s) you use; leave the rest empty):
```c
#define DEFAULT_WIFI_SSID   "YourWiFiName"            // 2.4 GHz network
#define DEFAULT_WIFI_PASS   "YourWiFiPassword"
#define ANTHROPIC_API_KEY   "sk-ant-...your key..."   // or OPENAI/GEMINI/OLLAMA below
#define OPENAI_API_KEY      ""
#define GEMINI_API_KEY      ""
#define OLLAMA_HOST         ""                         // e.g. "192.168.1.50:11434"
#define DEFAULT_AI_PROVIDER "anthropic"               // anthropic|openai|gemini|ollama
#define GEOAPIFY_KEY        ""                         // optional (maps)
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
- **Type a message + Enter** on the keyboard to chat.
- **Open Settings:** press the **trackball center button** (touch coming soon).
  Roll to move, click to open a category / change a value, **< Back** to go up,
  **Back to chat** to exit. Categories: **Display · Colors · AI Provider ·
  System / About**.
- **Switch AI provider:** Settings → **AI Provider** → click a provider. It shows
  `ready`/`no key` per provider and a `*` on the active one; switching first
  checks the provider actually answers (and pings a local Ollama) before
  activating — so you never get stranded on a dead provider.
- **Scroll** the chat: roll the trackball up/down.
- **On-device commands** (type in the chat, abbreviations OK):
  `/font small|medium|large` · `/inputfont ...` · `/color user|ai <name>` ·
  `/scroll <n>` · `/splash <ms>` · `/provider anthropic|openai|gemini|ollama` ·
  `/model <name>` · `/settings` · `/set`, `/fon`, `/col` …

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
