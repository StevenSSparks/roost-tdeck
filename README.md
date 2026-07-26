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
  per-category sub-pages — **Apps**, **Display**, **Colors**, **AI Provider**,
  **Device**, **System / About** — each with a Back item.
- **On-screen maps** 🗺️ — Geoapify static maps rendered on the LCD; center on
  GPS or any coordinates (`/map <lat> <lon>`). The AI can **show you a map** by
  calling a `show_map` tool (e.g. *"show me a map of Paris"*).
- **GPS** (L76K) for location + time; **NTP** time sync so the AI is time-aware.
- **Games** 🐍 — Snake (trackball/WASD), Sudoku (touch: tap a cell + number pad,
  10 boards), and a Slide 1-11 tile puzzle.
- **Touchscreen** — calibratable (Settings → Device → Calibrate touch, or
  `/calibrate`): tap the top bar for the menu, tap Settings rows, tap to play.
- **Configure from a terminal** — chat with the AI and change every setting over
  **USB-C serial** or a **network shell** (see below). Flash with just WiFi, then
  paste your API keys over the shell — no rebuild.
- **Personalization**: your name (the AI greets you), timezone, backlight
  brightness, sounds, trackball on/off, and more — all **saved** (survive reboot).
- Smooth, **adjustable fonts** and **configurable colors**, **scrollback**.
- Physical QWERTY input; trackball to scroll + navigate. (Touch calibration and
  real SSH are on the roadmap below.)

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

## Configure from a terminal (chat + settings, no rebuild)
The Communicator is also an **AI chat app you can drive from a terminal** — great
for advanced setup or pasting long API keys. Two ways in:

**USB-C (always works):** plug in and open a serial monitor:
```sh
pio device monitor -e app      # or: screen /dev/cu.usbmodem101 115200
```
**Network shell (plain TCP):** turn on *Settings → System → Remote shell*, then
from any computer **on the same network**:
```sh
nc <device-ip> 23              # or: telnet <device-ip>
```
**Real SSH (encrypted):** turn on *Settings → System → SSH server* (or `/ssh on`),
then:
```sh
ssh roost@<device-ip>          # default password: roostos
```
Change the login with `/sshuser <name>` and `/sshpass <password>`. The device
generates its own ED25519 host key on first boot (stored in NVS). You get the
same chat + `/set` shell, now over SSH.
Then, in either:
- **Just type** to chat with the AI (shared with the on-screen chat).
- `/set` — interactive **setup wizard** (name, timezone, API keys, provider,
  WiFi, brightness…). `/get` shows current config. `/help` lists commands.
- `/key anthropic sk-ant-…` · `/provider gemini` · `/wifi <ssid> <pass>` ·
  `/mapkey <key>` · `/name <you>` · `/map <lat> <lon>` · `/snake` · `/sudoku`

> Typical flow for a fresh board: flash with only your WiFi set, boot, connect
> USB-C (or `nc` if reachable), run `/set`, paste your keys — done, no rebuild.
> **Network shell note:** your Wi-Fi must allow client-to-client traffic (disable
> "AP/client isolation" on the SSID). USB-C works regardless.

### Performance & signal notes (ESP32 realities)
- **Turn SSH/remote-shell off for daily handheld use.** The SSH server runs in its
  own task (crypto + a listener); switching it off frees CPU/RAM and improves
  battery and UI responsiveness. Flip it on when you want to connect.
- **Load couples to WiFi range.** Under heavy load the ESP32's receiver gets less
  sensitive (self-interference), so a busy loop can *cost you signal bars*. The
  firmware keeps the UI loop light (throttled touch polling) partly for this reason;
  if SSH handshakes are flaky, a weak antenna and/or high load is usually why.
- **Antenna matters most.** The T-Deck Plus uses an external WiFi antenna on a U.FL
  connector — if RSSI is very low (e.g. −80 dBm right next to the router while a
  laptop shows −45), check that the WiFi antenna is seated (and not swapped with the
  LoRa one). SSH's key exchange is packet-heavy and needs a decent link; the plain
  TCP shell (`nc … 23`) tolerates a weaker one.

## Build environments
| Env | What |
|-----|------|
| `app`   | The on-screen firmware (this is the device). |
| `smoke` | A serial/TCP dev shell (WiFi/`ask`/`gps`/`bat`) — handy for bring-up. |
| `native`| Off-device unit tests: `pio test -e native`. |

## Roadmap
**Done:** multi-provider chat · categorized + scrolling Settings · on-screen maps
+ AI `show_map` · GPS + NTP time-awareness · terminal chat/config over USB-C and
TCP · real **SSH** server · runtime key config (no rebuild) · **working touch**
(GT911 fix + calibration) · Snake, Sudoku (touch), Slide 1-11 · on-device WiFi +
personalization.

**Next:** swipe-to-scroll chat via touch · turn-by-turn **directions** · battery
ADC calibration · custom boot artwork · WiFi auto-reconnect · web search · SSH
key-based auth. See `MORNING.md` and `docs/superpowers/plans/` for the working plan.

## Safety
No secrets are ever committed. Stock firmware can be backed up before flashing
(`backups/`). This overwrites the LILYGO factory firmware — keep a backup if you
want to restore it.
