// RoostOS Communicator — on-screen app.
// ST7789 320x240 via TFT_eSPI, physical keyboard (I2C 0x55), WiFi + Claude Haiku.
// On-screen chat with smooth fonts, configurable sizes/colors, scrollback.
// Serial dev commands: ask/claude <t> | font <n> | inputfont <n> | color user|ai <name> | ip
// Built by [env:app]. Uses include/secrets.h (DEV_SECRETS).
// NOTE: all of these live-config values become persisted Settings later (NVS).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <vector>
#include <functional>
#include "secrets.h"
#include "version.h"

static Preferences prefs;
static uint8_t gtAddr = 0;   // GT911 touch controller address (0 = not found)

// ---- pins ----
#define PIN_POWERON 10
#define PIN_BL      42
#define KB_ADDR     0x55
#define I2C_SDA     18
#define I2C_SCL     8
// Trackball direction pins (from LILYGO T-Deck UnitTest mouse_read):
//   UP=BOARD_TBOX_G01(GPIO3)  DOWN=BOARD_TBOX_G03(GPIO15)
//   RIGHT=BOARD_TBOX_G02(GPIO2)  LEFT=BOARD_TBOX_G04(GPIO1)
#define TB_UP       3    // scroll up
#define TB_DOWN     15   // scroll down

// Fallback defaults so an older/partial secrets.h still compiles
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY ""
#endif
#ifndef OLLAMA_HOST
#define OLLAMA_HOST ""
#endif
#ifndef DEFAULT_AI_PROVIDER
#define DEFAULT_AI_PROVIDER "anthropic"
#endif
#ifndef GEOAPIFY_KEY
#define GEOAPIFY_KEY ""
#endif
#ifndef SSH_USER
#define SSH_USER "roost"
#endif
#ifndef SSH_PASS
#define SSH_PASS "roostos"
#endif
#ifndef ANTHROPIC_API_KEY
#define ANTHROPIC_API_KEY ""
#endif

static const char* SYS_PROMPT =
  "You are RoostOS, a friendly assistant on a small handheld device with a tiny "
  "pixel-font screen. Reply in plain ASCII text only: no markdown, no headings, "
  "no bullet symbols, no emoji, no special/unicode characters. Keep replies short.";

static const char* PROVIDERS[] = {"anthropic", "openai", "gemini", "ollama"};
static const int NPROV = 4;
static String defaultModel(const String& p) {
  if (p == "openai") return "gpt-4o-mini";
  if (p == "gemini") return "gemini-flash-lite-latest";   // cheap alias, won't go stale
  if (p == "ollama") return "llama3.2";
  return "claude-haiku-4-5";
}

// API keys/host: default from secrets.h but RUNTIME-OVERRIDABLE (paste over SSH /
// serial, stored in NVS) so anyone can flash with only WiFi and configure later.
static String  kAnthropic = ANTHROPIC_API_KEY;
static String  kOpenAI    = OPENAI_API_KEY;
static String  kGemini    = GEMINI_API_KEY;
static String  ollamaHost = OLLAMA_HOST;

// A provider is selectable only if its key/host is configured.
static bool providerConfigured(const String& p) {
  if (p == "anthropic") return kAnthropic.length() > 0;
  if (p == "openai")    return kOpenAI.length() > 0;
  if (p == "gemini")    return kGemini.length() > 0;
  if (p == "ollama")    return ollamaHost.length() > 0;
  return false;
}

TFT_eSPI tft = TFT_eSPI();
static uint16_t C_BG, C_PANEL, C_INK, C_DIM, C_TEAL, C_INDIGO, C_AMBER;

// ---- font choices ----
struct FontChoice { const GFXfont* gfx; const char* name; };  // gfx==nullptr => tiny GLCD
static const FontChoice FONTS[] = {
  { nullptr,         "tiny"   },  // 0
  { &FreeSans9pt7b,  "small"  },  // 1
  { &FreeSans12pt7b, "medium" },  // 2
  { &FreeSans18pt7b, "large"  },  // 3
};
static const int NFONTS = 4;
static void setFont(int idx) {
  const GFXfont* g = FONTS[idx].gfx;
  if (g) tft.setFreeFont(g); else { tft.setTextFont(1); tft.setTextSize(1); }
}

// ---- configurable state (future Settings) ----
static int chatFontIdx  = 1;   // message text size (default small = FreeSans 9pt)
static int inputFontIdx = 1;   // input-box text size (default small)
static uint16_t userColor, aiColor, accentColor;   // set in setup(); configurable
static String  youLabel = "You";      // chat label for your own messages (name/initials)
static int splashMs   = 3500;  // boot splash duration
static int scrollStep = 2;     // lines per trackball detent (adjustable "rate")
static String aiProvider = DEFAULT_AI_PROVIDER;   // anthropic|openai|gemini|ollama
static String aiModel = "";                        // set from NVS / provider default

// ---- device / personalization / feature config (all persisted) ----
static String  userName   = "";                    // AI addresses the user by this
static int     tzOffsetMin = 0;                    // minutes from UTC (e.g. -300 = EST)
static int     brightness = 230;                   // 0..255 backlight (LEDC PWM on BL)
static bool    soundsOn   = true;                  // UI/alert tones (speaker)
static bool    trackballOn = true;                 // false hides the cursor/roll nav
static bool    webSearchOn = false;                // allow AI web search (roadmap)
static bool    toolShowMap = true, toolGetLoc = true;   // AI tools the model may call
static String  promptWord = "roostos";             // shell prompt word (roostos/ai/os/…)
static bool    remoteShellOn = false;              // TCP shell on port 23
// Status-bar mode: IP = SSID+IP; DEMO = masked ("Demo") for photos; PHONE = wifi
// bars + battery + clock (no SSID/IP). Each renders differently.
enum { STAT_IP, STAT_DEMO, STAT_PHONE };
static int     statusMode = STAT_IP;
// SSH: default ON only if creds were configured in secrets; else off (safe default).
static bool    sshOn = (SSH_USER[0] != '\0' && SSH_PASS[0] != '\0');
static String  sshUser = SSH_USER, sshPass = SSH_PASS;   // SSH login (from secrets, editable)
static String  mapKey     = GEOAPIFY_KEY;          // Geoapify static-map key
// WiFi override set on-device (empty => fall back to secrets.h DEFAULT_WIFI_*)
static String  wifiSsid = "", wifiPass = "";
// last-known location (from GPS or a manual/AI set); used by the map screen
static double  locLat = 0, locLon = 0; static bool locValid = false;
// AI-requested map (set when the model calls the show_map tool; rendered after reply)
static bool    pendingMap = false; static double pMapLat = 0, pMapLon = 0;
// last map shown (persisted) so /map can reopen it without a GPS fix
static double  lastMapLat = 0, lastMapLon = 0; static bool lastMapValid = false;
// user-set home location (persisted); a /map fallback when there's no GPS fix
static double  homeLat = 0, homeLon = 0; static bool homeValid = false;
// touch calibration: screen = affine(raw). Defaults to identity (uncalibrated).
static float   tcAx = 1, tcBx = 0, tcCx = 0, tcAy = 0, tcBy = 1, tcCy = 0;
static bool    touchCalValid = false;
static TinyGPSPlus gps;
static HardwareSerial GPSser(1);
// TJpg_Decoder -> TFT block callback (defined early; used in setup + showMap)
static bool jpgToTft(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bmp);
  return 1;
}

static void applyBrightness() {
  // LEDC PWM on the backlight pin (Arduino-ESP32 2.x API)
  static bool init = false;
  if (!init) { ledcSetup(0, 5000, 8); ledcAttachPin(PIN_BL, 0); init = true; }
  ledcWrite(0, constrain(brightness, 8, 255));
}
// Network name for display — masked to "Demo" when demo mode is on (for photos).
static String dispSsid() { return statusMode != STAT_IP ? String("Demo") : WiFi.SSID(); }
static const char* statusName() { return statusMode == STAT_IP ? "IP" : statusMode == STAT_DEMO ? "Demo" : "Phone"; }
// battery percent from the GPIO4 ADC (board divider ~x2; 3.3V=0%, 4.2V=100%)
static int batteryPct() {
  uint32_t mv = analogReadMilliVolts(4) * 2;
  int pct = (int)(((long)mv - 3300) * 100 / (4200 - 3300));
  return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}
// Approximate charging detection: on USB the pack reads high (no dedicated pin).
static bool batteryCharging() { return analogReadMilliVolts(4) * 2 > 4250; }
// wifi signal level 0..4 from RSSI
static int wifiLevel() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int r = WiFi.RSSI();
  return r > -55 ? 4 : r > -65 ? 3 : r > -72 ? 2 : r > -82 ? 1 : 0;
}

static void saveCfg() {
  prefs.begin("roostcomm", false);
  prefs.putInt("chatFont", chatFontIdx);
  prefs.putInt("inputFont", inputFontIdx);
  prefs.putUShort("userCol", userColor);
  prefs.putUShort("accentCol", accentColor);
  prefs.putString("youLabel", youLabel);
  prefs.putUShort("aiCol", aiColor);
  prefs.putInt("splashMs", splashMs);
  prefs.putInt("scrollStep", scrollStep);
  prefs.putString("aiProv", aiProvider);
  prefs.putString("aiModel", aiModel);
  prefs.putString("userName", userName);
  prefs.putInt("tzOff", tzOffsetMin);
  prefs.putInt("bright", brightness);
  prefs.putBool("sounds", soundsOn);
  prefs.putBool("tball", trackballOn);
  prefs.putBool("websrch", webSearchOn);
  prefs.putBool("tShowMap", toolShowMap);
  prefs.putBool("tGetLoc", toolGetLoc);
  prefs.putString("prompt", promptWord);
  prefs.putBool("rshell", remoteShellOn);
  prefs.putString("mapKey", mapKey);
  prefs.putString("wSsid", wifiSsid);
  prefs.putString("wPass", wifiPass);
  prefs.putString("kAnth", kAnthropic);
  prefs.putString("kOai", kOpenAI);
  prefs.putString("kGem", kGemini);
  prefs.putString("olHost", ollamaHost);
  prefs.putInt("statMode", statusMode);
  prefs.putBool("sshOn", sshOn);
  prefs.putString("sshUser", sshUser);
  prefs.putString("sshPass", sshPass);
  prefs.putDouble("mapLat", lastMapLat);
  prefs.putDouble("mapLon", lastMapLon);
  prefs.putBool("mapVal", lastMapValid);
  prefs.putDouble("homeLat", homeLat);
  prefs.putDouble("homeLon", homeLon);
  prefs.putBool("homeVal", homeValid);
  prefs.putBool("tcVal2", touchCalValid);
  prefs.putFloat("tcAx", tcAx); prefs.putFloat("tcBx", tcBx); prefs.putFloat("tcCx", tcCx);
  prefs.putFloat("tcAy", tcAy); prefs.putFloat("tcBy", tcBy); prefs.putFloat("tcCy", tcCy);
  prefs.end();
}
static void loadCfg() {   // call AFTER colors + defaults are set
  prefs.begin("roostcomm", true);
  chatFontIdx  = prefs.getInt("chatFont", chatFontIdx);
  inputFontIdx = prefs.getInt("inputFont", inputFontIdx);
  userColor    = prefs.getUShort("userCol", userColor);
  accentColor  = prefs.getUShort("accentCol", accentColor);
  youLabel     = prefs.getString("youLabel", youLabel);
  aiColor      = prefs.getUShort("aiCol", aiColor);
  splashMs     = prefs.getInt("splashMs", splashMs);
  scrollStep   = prefs.getInt("scrollStep", scrollStep);
  aiProvider   = prefs.getString("aiProv", aiProvider);
  aiModel      = prefs.getString("aiModel", defaultModel(aiProvider));
  userName     = prefs.getString("userName", userName);
  tzOffsetMin  = prefs.getInt("tzOff", tzOffsetMin);
  brightness   = prefs.getInt("bright", brightness);
  soundsOn     = prefs.getBool("sounds", soundsOn);
  trackballOn  = prefs.getBool("tball", trackballOn);
  webSearchOn  = prefs.getBool("websrch", webSearchOn);
  toolShowMap  = prefs.getBool("tShowMap", toolShowMap);
  toolGetLoc   = prefs.getBool("tGetLoc", toolGetLoc);
  promptWord   = prefs.getString("prompt", promptWord);
  remoteShellOn = prefs.getBool("rshell", remoteShellOn);
  mapKey       = prefs.getString("mapKey", mapKey);
  wifiSsid     = prefs.getString("wSsid", wifiSsid);
  wifiPass     = prefs.getString("wPass", wifiPass);
  kAnthropic   = prefs.getString("kAnth", kAnthropic);
  kOpenAI      = prefs.getString("kOai", kOpenAI);
  kGemini      = prefs.getString("kGem", kGemini);
  ollamaHost   = prefs.getString("olHost", ollamaHost);
  statusMode   = prefs.getInt("statMode", statusMode);
  sshOn        = prefs.getBool("sshOn", sshOn);
  sshUser      = prefs.getString("sshUser", sshUser);
  sshPass      = prefs.getString("sshPass", sshPass);
  lastMapLat   = prefs.getDouble("mapLat", 0);
  lastMapLon   = prefs.getDouble("mapLon", 0);
  lastMapValid = prefs.getBool("mapVal", false);
  homeLat      = prefs.getDouble("homeLat", 0);
  homeLon      = prefs.getDouble("homeLon", 0);
  homeValid    = prefs.getBool("homeVal", false);
  touchCalValid = prefs.getBool("tcVal2", false);
  if (touchCalValid) {
    tcAx = prefs.getFloat("tcAx", 1); tcBx = prefs.getFloat("tcBx", 0); tcCx = prefs.getFloat("tcCx", 0);
    tcAy = prefs.getFloat("tcAy", 0); tcBy = prefs.getFloat("tcBy", 1); tcCy = prefs.getFloat("tcCy", 0);
  }
  prefs.end();
}

// named color lookup
static uint16_t namedColor(const String& n) {
  if (n == "teal")    return C_TEAL;
  if (n == "indigo" || n == "blue") return C_INDIGO;
  if (n == "amber" || n == "yellow") return C_AMBER;
  if (n == "ink" || n == "white")    return C_INK;
  if (n == "dim" || n == "gray")     return C_DIM;
  if (n == "red")     return tft.color565(0xff, 0x5a, 0x5a);
  if (n == "green")   return tft.color565(0x5a, 0xff, 0x8a);
  if (n == "cyan")    return tft.color565(0x4d, 0xe6, 0xff);
  if (n == "magenta" || n == "pink") return tft.color565(0xff, 0x6a, 0xd5);
  if (n == "orange")  return tft.color565(0xff, 0x9a, 0x3d);
  if (n == "purple")  return tft.color565(0xb0, 0x7d, 0xff);
  if (n == "sky")     return tft.color565(0x5a, 0xc8, 0xff);
  if (n == "lime")    return tft.color565(0xc6, 0xff, 0x4d);
  if (n == "rose")    return tft.color565(0xff, 0x6a, 0x8a);
  if (n == "gold")    return tft.color565(0xff, 0xd2, 0x4d);
  if (n == "mint")    return tft.color565(0x6a, 0xff, 0xc2);
  return 0xFFFF;  // 0xFFFF = "unknown"
}

// ---- screen modes ----
enum { MODE_CHAT, MODE_SETTINGS, MODE_ABOUT, MODE_TEXT, MODE_WIFI, MODE_MAP, MODE_GAME, MODE_CALIB };
static int uiMode = MODE_CHAT;
static int selIdx = 0;
// Settings are organized as a main page with per-category sub-pages (BlackBerry
// style: <=6 options each, item 0 is always Back). selIdx indexes the current page.
enum { PG_MAIN, PG_APPS, PG_DISPLAY, PG_COLORS, PG_AI, PG_DEVICE, PG_SYSTEM, PG_SSH, PG_COUNT };
static int setPage = PG_MAIN;
static int setFirst = 0;     // first visible row (settings scroll window)
static String setMsg = "";   // transient status line (e.g. provider switch result)
static const char* PAL_NAMES[] = {"teal","indigo","amber","red","green","cyan","magenta","orange",
                                  "purple","sky","lime","rose","gold","mint","ink"};
static const int NPAL = 15;
static int palIndexOf(uint16_t c) { for (int i = 0; i < NPAL; i++) if (namedColor(PAL_NAMES[i]) == c) return i; return 0; }
#define TB_CLICK 0   // trackball center button (BOARD_BOOT_PIN / GPIO0)

// ---- chat state ----
struct Msg { String text; uint16_t color; };
static std::vector<Msg> msgs;
static String input;
static int scrW, scrH;
static const int headerH = 15;
static int scrollLines = 0;               // lines scrolled up from bottom (0=latest)
static int lastTotalLines = 0, lastRows = 0;

static void addMsg(const String& t, uint16_t color) {
  msgs.push_back({t, color});
  while (msgs.size() > 300) msgs.erase(msgs.begin());
}

// wrap one message to fit maxW px in the CURRENT font
static void wrapMsg(const String& text, int maxW, std::vector<String>& out) {
  String line; int i = 0, n = text.length();
  while (i < n) {
    int j = i; while (j < n && text[j] != ' ' && text[j] != '\n') j++;
    String word = text.substring(i, j);
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial) <= maxW) line = trial;
    else {
      if (line.length()) { out.push_back(line); line = ""; }
      if (tft.textWidth(word) > maxW) {                 // hard-break a too-long word
        String part;
        for (int k = 0; k < (int)word.length(); k++) {
          String t2 = part + word[k];
          if (tft.textWidth(t2) > maxW && part.length()) { out.push_back(part); part = String(word[k]); }
          else part = t2;
        }
        line = part;
      } else line = word;
    }
    if (j < n && text[j] == '\n') { out.push_back(line); line = ""; }
    i = j + 1;
  }
  out.push_back(line);   // always emit the final line (this was the "You:"-disappearing bug)
}

static void draw() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);

  // header (compact GLCD)
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  int rx = 4 + tft.textWidth("RoostOS");
  // per-mode title (always starts with RoostOS)
  const char* suffix = (statusMode == STAT_DEMO) ? " AI Communicator"
                     : (statusMode == STAT_PHONE) ? " AI" : " Communicator";
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(suffix, rx, 4);
  // menu button (hamburger) top-right — tap to open Settings
  for (int b = 0; b < 3; b++) tft.fillRect(scrW - 20, 3 + b * 4, 15, 2, C_AMBER);
  int rEdge = scrW - 24;   // content ends left of the menu button
  auto drawBars = [&](int x) {                      // 4 wifi bars at [x..x+15]
    int lvl = wifiLevel();
    for (int i = 0; i < 4; i++) { int h = 3 + i * 2;
      tft.fillRect(x + i * 4, 11 - h, 3, h, i < lvl ? C_TEAL : C_PANEL);
      tft.drawRect(x + i * 4, 11 - h, 3, h, i < lvl ? C_TEAL : C_DIM); }
  };
  auto drawBatt = [&](int rightX) -> int {          // battery pill; returns its left x
    int bpct = batteryPct(); bool chg = batteryCharging();
    int w = 18, h = 9, x = rightX - w, y = 3;
    tft.drawRect(x, y, w, h, C_DIM); tft.fillRect(x + w, y + 2, 2, h - 4, C_DIM);  // nub
    int fill = (w - 2) * bpct / 100;
    tft.fillRect(x + 1, y + 1, fill, h - 2, chg ? C_TEAL : (bpct <= 15 ? C_AMBER : C_INK));
    if (chg) { tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("+", x - 6, 4); }  // charging
    return x - (chg ? 8 : 2);
  };
  tft.setTextColor(C_DIM, C_PANEL);
  if (statusMode == STAT_IP) {                      // SSID + IP (dev/info)
    String s = WiFi.status() == WL_CONNECTED ? (WiFi.SSID() + " " + WiFi.localIP().toString()) : String("WiFi down");
    tft.drawString(s, rEdge - tft.textWidth(s), 4);
  } else if (statusMode == STAT_DEMO) {             // bars + battery (no SSID/IP) — photo-friendly
    int bx = rEdge - 16; drawBars(bx);
    drawBatt(bx - 6);
  } else {                                          // PHONE: clock + battery + bars
    int bx = rEdge - 16; drawBars(bx);
    int batLeft = drawBatt(bx - 6);
    time_t nowt = time(nullptr);
    if (nowt > 1700000000) { nowt += (time_t)tzOffsetMin * 60; struct tm tmv; gmtime_r(&nowt, &tmv);
      char c[8]; strftime(c, sizeof(c), "%H:%M", &tmv);
      tft.setTextColor(C_DIM, C_PANEL); tft.drawString(c, batLeft - 4 - tft.textWidth(c), 4); }
  }

  // input-box metrics (its own font)
  setFont(inputFontIdx);
  int inputH = tft.fontHeight() + 6;
  int inputY = scrH - inputH;

  // chat area
  setFont(chatFontIdx);
  int lh = tft.fontHeight() + 2;
  int top = headerH + 3;
  int maxW = scrW - 4;
  std::vector<std::pair<String, uint16_t>> wl;
  for (auto& m : msgs) {
    std::vector<String> ls; wrapMsg(m.text, maxW, ls);
    for (auto& s : ls) wl.push_back({s, m.color});
  }
  int rows = (inputY - top) / lh;
  lastTotalLines = wl.size(); lastRows = rows;
  int maxScroll = wl.size() > rows ? (int)wl.size() - rows : 0;
  if (scrollLines < 0) scrollLines = 0;
  if (scrollLines > maxScroll) scrollLines = maxScroll;
  int end = (int)wl.size() - scrollLines;
  int start = end - rows; if (start < 0) start = 0;
  int y = top;
  for (int i = start; i < end && i < (int)wl.size(); i++) {
    tft.setTextColor(wl[i].second, C_BG);
    tft.drawString(wl[i].first, 2, y); y += lh;
  }
  // scroll indicators: ^ = more above, v = more below
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_AMBER, C_BG);
  if (start > 0)              tft.drawString("^", scrW - 12, top);
  if (end < (int)wl.size())   tft.drawString("v", scrW - 12, inputY - 12);

  // input line (own font, amber prompt) + a tappable clear-chat button
  const int cbw = 34;
  tft.fillRect(0, inputY, scrW, inputH, C_PANEL);
  setFont(inputFontIdx);
  tft.setTextColor(accentColor, C_PANEL); tft.drawString("> ", 2, inputY + 2);
  int pw = tft.textWidth("> ") + 2;
  tft.setTextColor(C_INK, C_PANEL);
  String shown = input;
  while (shown.length() && tft.textWidth(shown) > maxW - pw - cbw) shown = shown.substring(1);
  tft.drawString(shown, 2 + pw, inputY + 2);
  // clear button (bottom-right)
  tft.drawRoundRect(scrW - cbw, inputY + 1, cbw - 2, inputH - 2, 3, C_DIM);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_PANEL); tft.drawString("clr", scrW - cbw / 2 - 1, inputY + inputH / 2);
  tft.setTextDatum(TL_DATUM);
}

static void clearChat() {
  msgs.clear(); scrollLines = 0; input = "";
  addMsg("Ready to Roost! Type a message + Enter, or tap the menu.", C_TEAL);
  draw();
}

// ---- Settings screen: main page + per-category sub-pages ----
// forward decls (defined later in the file)
static bool switchProvider(const String& p, String& msg);
static bool providerConfigured(const String& p);
static String defaultModel(const String& p);
static bool joinWifi(const String& ssid, const String& pass);
static String activeSsid();
static void draw();
static void showMap(double lat, double lon, int zoom);
static void gameLaunch(int which);
static void startCalibration();
void sshRegenHostKey();

// Fill labels[]/values[] for a page and return its title. Item 0 is always Back.
static String buildPage(int pg, std::vector<String>& labels, std::vector<String>& values) {
  labels.clear(); values.clear();
  auto row = [&](const String& l, const String& v){ labels.push_back(l); values.push_back(v); };
  switch (pg) {
    case PG_MAIN:
      row("Back to chat", "");
      row("Apps", ">");
      row("Display", ">"); row("Colors", ">");
      row("AI Provider", aiProvider);
      row("Device", ">");
      row("SSH", sshOn ? "on :22" : "off");
      row("System / About", ">");
      return "Settings";
    case PG_APPS:
      row("< Back", "");
      row("Map", locValid ? "gps" : (lastMapValid ? "saved" : "no fix"));
      row("Snake", ">");
      row("Sudoku", ">");
      row("Slide 1-11", ">");
      row("GPS status", locValid ? String(locLat, 3) + "," + String(locLon, 3) : String("no fix"));
      return "Apps";
    case PG_DEVICE:
      row("< Back", "");
      row("Your name", userName.length() ? userName : String("(set)"));
      row("Chat label", youLabel);
      row("Timezone", (tzOffsetMin >= 0 ? "+" : "-") + String(abs(tzOffsetMin) / 60) + "h");
      row("Brightness", String((brightness * 100) / 255) + "%");
      row("Sounds", soundsOn ? "on" : "off");
      row("Trackball", trackballOn ? "on" : "off");
      row("Web search", webSearchOn ? "on" : "off");
      row("Status bar", statusName());
      row("Calibrate touch", touchCalValid ? "done" : "needed");
      return "Device";
    case PG_DISPLAY:
      row("< Back", "");
      row("Chat font",  FONTS[chatFontIdx].name);
      row("Input font", FONTS[inputFontIdx].name);
      row("Scroll rate", String(scrollStep));
      row("Splash", String(splashMs / 1000.0, 1) + "s");
      return "Display";
    case PG_COLORS:
      row("< Back", "");
      row(youLabel + " color", PAL_NAMES[palIndexOf(userColor)]);
      row("AI color",   PAL_NAMES[palIndexOf(aiColor)]);
      row("Accent/buttons", PAL_NAMES[palIndexOf(accentColor)]);
      return "Colors";
    case PG_AI: {
      row("< Back", "");
      for (int k = 0; k < NPROV; k++) {
        String p = PROVIDERS[k];
        String tag = (p == aiProvider ? "* " : "  ");
        tag += providerConfigured(p) ? "ready" : "no key";
        row(p == aiProvider ? ("[" + p + "]").c_str() : p.c_str(), tag);
      }
      row("Model", aiModel.length() ? aiModel : defaultModel(aiProvider));
      return "AI Provider";
    }
    case PG_SYSTEM:
      row("< Back", "");
      row("About", ">");
      row("WiFi setup", WiFi.isConnected() ? dispSsid() : String("down"));
      row("Map key", strlen(mapKey.c_str()) ? "set" : "none");
      row("IP", WiFi.localIP().toString());
      row("Uptime", String(millis() / 1000) + "s");
      return "System";
    case PG_SSH:
      row("< Back", "");
      row("SSH server", sshOn ? "on :22" : "off");
      row("TCP shell", remoteShellOn ? "on :23" : "off");
      row("User", sshUser.length() ? sshUser : String("(unset)"));
      row("Password", sshPass.length() ? sshPass : String("(unset)"));
      row("Connect", WiFi.isConnected() ? ("ssh " + (sshUser.length() ? sshUser : String("user")) + "@" + WiFi.localIP().toString()) : String("wifi down"));
      row("Regen host key", ">");
      return "Remote / SSH";
  }
  return "Settings";
}

static int pageLen(int pg) {
  std::vector<String> l, v; buildPage(pg, l, v); return (int)l.size();
}

static void drawSettings() {
  std::vector<String> labels, values;
  String title = buildPage(setPage, labels, values);
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(" " + title, 4 + tft.textWidth("RoostOS"), 4);
  setFont(1);   // small
  int lh = tft.fontHeight() + 6, y0 = headerH + 6;
  int n = (int)labels.size();
  int availH = (scrH - 14) - y0;
  int rowsVis = availH / lh; if (rowsVis < 1) rowsVis = 1;
  // scroll window so the selected row stays visible (handles long pages)
  int first = 0;
  if (n > rowsVis) { if (selIdx >= rowsVis) first = selIdx - rowsVis + 1;
                     if (first > n - rowsVis) first = n - rowsVis; if (first < 0) first = 0; }
  setFirst = first;
  int y = y0;
  for (int i = first; i < first + rowsVis && i < n; i++) {
    bool sel = (i == selIdx);
    if (sel) tft.fillRect(0, y - 2, scrW, lh, C_PANEL);
    uint16_t bg = sel ? C_PANEL : C_BG;
    tft.setTextColor(sel ? C_AMBER : C_INK, bg);
    tft.drawString(labels[i], 6, y);
    // color rows: draw a real swatch (WYSIWYG) + the name, so selection is clear
    bool colorRow = (setPage == PG_COLORS && i >= 1 && i <= 3);
    if (colorRow) {
      uint16_t sc = (i == 1) ? userColor : (i == 2) ? aiColor : accentColor;
      int sw = 26, sh = lh - 8, sxp = scrW - sw - 8;
      tft.fillRect(sxp, y, sw, sh, sc); tft.drawRect(sxp, y, sw, sh, C_DIM);
      tft.setTextColor(sel ? C_INK : C_DIM, bg);
      tft.drawString(values[i], sxp - tft.textWidth(values[i]) - 6, y);
    } else if (values[i].length()) {
      tft.setTextColor(sel ? C_INK : C_DIM, bg);
      tft.drawString(values[i], scrW - tft.textWidth(values[i]) - 8, y);
    }
    y += lh;
  }
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_AMBER, C_BG);
  if (first > 0)              tft.drawString("^", scrW - 12, y0);
  if (first + rowsVis < n)    tft.drawString("v", scrW - 12, scrH - 24);
  tft.setTextColor(C_DIM, C_BG);
  if (setMsg.length()) tft.drawString(setMsg, 4, scrH - 12);
  else tft.drawString(setPage == PG_MAIN ? "ball: roll=move  click=open  (or /set)"
                                         : "ball: roll=move  click=change  < Back to return", 4, scrH - 12);
}

static void drawAbout() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(" About", 4 + tft.textWidth("RoostOS"), 4);
  setFont(0);   // tiny GLCD so all rows fit without scrolling
  int y = headerH + 5, lh = tft.fontHeight() + 4;
  auto row = [&](const String& a, const String& b, uint16_t bc = 0) {
    tft.setTextColor(C_DIM, C_BG); tft.drawString(a, 6, y);
    tft.setTextColor(bc ? bc : C_INK, C_BG); tft.drawString(b, 88, y); y += lh;
  };
  row("Product", "RoostOS Communicator", C_TEAL);
  row("Web", "roostos.dev/tdeck", C_AMBER);
  row("GitHub", "github.com/StevenSSparks/roost-tdeck", C_AMBER);
  row("Version", ROOST_COMM_VERSION);
  row("Device", "T-Deck Plus (S3)");
  row("MAC", WiFi.macAddress());
  row("IP", WiFi.localIP().toString());
  row("WiFi", dispSsid() + " " + String(WiFi.RSSI()) + "dBm");
  row("Status bar", statusName());
  row("Heap/PSRAM", String(ESP.getFreeHeap() / 1024) + "K / " + String(ESP.getFreePsram() / 1024) + "K");
  row("Uptime", String(millis() / 1000) + "s");
  row("Touch", gtAddr ? "GT911 0x" + String(gtAddr, HEX) : "none");
  tft.setTextColor(C_AMBER, C_BG); tft.drawString("tap / any key = back", 4, scrH - 12);
}

// pick the next model in a small per-provider preset ring
static String nextModel(const String& p, const String& cur) {
  const char* an[] = {"claude-haiku-4-5", "claude-sonnet-4-6"};
  const char* oa[] = {"gpt-4o-mini", "gpt-4o"};
  const char* ge[] = {"gemini-flash-lite-latest", "gemini-2.5-flash"};
  const char* ol[] = {"llama3.2", "qwen2.5", "phi3"};
  const char** L; int n;
  if (p == "openai")      { L = oa; n = 2; }
  else if (p == "gemini") { L = ge; n = 2; }
  else if (p == "ollama") { L = ol; n = 3; }
  else                    { L = an; n = 2; }
  String c = cur.length() ? cur : defaultModel(p);
  int idx = 0; for (int i = 0; i < n; i++) if (c == L[i]) idx = i;
  return String(L[(idx + 1) % n]);
}

// ---- generic on-screen text entry (keyboard) ----
// Used for name, WiFi password, map key, map destination. Enter commits (calls
// the callback), Esc (27) cancels. Optional masking hides the value (passwords).
static String textTitle, textVal, textHint;
static bool   textMask = false;
static std::function<void(String)> textCb;
static int    textReturnMode = MODE_SETTINGS;

static void drawText() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  tft.setTextColor(C_INK, C_PANEL);  tft.drawString(" " + textTitle, 4 + tft.textWidth("RoostOS"), 4);
  // [X] cancel button (top-right) — the keyboard has no Esc key
  tft.drawRect(scrW - 16, 1, 14, headerH - 2, C_DIM);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(C_AMBER, C_PANEL);
  tft.drawString("X", scrW - 9, headerH / 2); tft.setTextDatum(TL_DATUM);
  setFont(1);
  int y = headerH + 18;
  tft.setTextColor(C_DIM, C_BG); tft.drawString(textHint.length() ? textHint : String("type, then Enter"), 8, y);
  y += tft.fontHeight() + 12;
  String shown = textVal;
  if (textMask) { shown = ""; for (size_t i = 0; i < textVal.length(); i++) shown += '*'; }
  tft.fillRect(6, y - 2, scrW - 12, tft.fontHeight() + 8, C_PANEL);
  tft.setTextColor(C_AMBER, C_PANEL); tft.drawString("> ", 10, y + 2);
  int pw = tft.textWidth("> ");
  tft.setTextColor(C_INK, C_PANEL);
  while (shown.length() && tft.textWidth(shown) > scrW - 30 - pw) shown = shown.substring(1);
  tft.drawString(shown + "_", 10 + pw, y + 2);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Enter = save    tap X / click ball = cancel", 4, scrH - 12);
}
static void openText(const String& title, const String& initial, const String& hint,
                     bool mask, int returnMode, std::function<void(String)> cb) {
  textTitle = title; textVal = initial; textHint = hint; textMask = mask;
  textReturnMode = returnMode; textCb = cb; uiMode = MODE_TEXT; drawText();
}

static void activateSetting() {
  setMsg = "";
  if (setPage == PG_MAIN) {
    switch (selIdx) {
      case 0: uiMode = MODE_CHAT; draw(); return;         // Back to chat
      case 1: setPage = PG_APPS;    selIdx = 0; break;
      case 2: setPage = PG_DISPLAY; selIdx = 0; break;
      case 3: setPage = PG_COLORS;  selIdx = 0; break;
      case 4: setPage = PG_AI;      selIdx = 0; break;
      case 5: setPage = PG_DEVICE;  selIdx = 0; break;
      case 6: setPage = PG_SSH;     selIdx = 0; break;
      case 7: setPage = PG_SYSTEM;  selIdx = 0; break;
    }
    drawSettings(); return;
  }
  // sub-pages: item 0 is Back to the main page
  if (selIdx == 0) { setPage = PG_MAIN; selIdx = 0; drawSettings(); return; }

  switch (setPage) {
    case PG_APPS:
      switch (selIdx) {
        case 1: if (locValid)     { showMap(locLat, locLon, 14); return; }        // GPS
                if (lastMapValid) { showMap(lastMapLat, lastMapLon, 14); return; } // last saved
                setMsg = "no location; use /map <lat> <lon>"; break;
        case 2: gameLaunch(0); return;   // Snake
        case 3: gameLaunch(1); return;   // Sudoku
        case 4: gameLaunch(2); return;   // Slide
        case 5: setMsg = locValid ? "gps fix ok" : "no fix (sats " + String(gps.satellites.value()) + ")"; break;
      }
      break;
    case PG_DISPLAY:
      switch (selIdx) {
        case 1: chatFontIdx  = (chatFontIdx + 1) % NFONTS; break;
        case 2: inputFontIdx = (inputFontIdx + 1) % NFONTS; break;
        case 3: scrollStep = scrollStep >= 8 ? 1 : scrollStep + 1; break;
        case 4: { int opts[] = {0, 1500, 3000, 5000}; int cur = 0;
                  for (int k = 0; k < 4; k++) if (opts[k] == splashMs) cur = k;
                  splashMs = opts[(cur + 1) % 4]; break; }
      }
      break;
    case PG_COLORS:
      if (selIdx == 1) userColor   = namedColor(PAL_NAMES[(palIndexOf(userColor)   + 1) % NPAL]);
      if (selIdx == 2) aiColor     = namedColor(PAL_NAMES[(palIndexOf(aiColor)     + 1) % NPAL]);
      if (selIdx == 3) accentColor = namedColor(PAL_NAMES[(palIndexOf(accentColor) + 1) % NPAL]);
      break;
    case PG_AI: {
      int modelRow = 1 + NPROV;   // rows 1..NPROV are providers, then Model
      if (selIdx >= 1 && selIdx <= NPROV) {
        String cand = PROVIDERS[selIdx - 1];
        if (cand == aiProvider) { setMsg = cand + " already active"; }
        else if (!providerConfigured(cand)) { setMsg = cand + ": no key configured"; }
        else { switchProvider(cand, setMsg); }   // verify-then-switch (pings local Ollama)
      } else if (selIdx == modelRow) {
        aiModel = nextModel(aiProvider, aiModel); saveCfg();
      }
      break;
    }
    case PG_DEVICE:
      switch (selIdx) {
        case 1:  // Your name -> text entry (used in the AI system prompt)
          openText("Your name", userName, "so the AI can greet you", false, MODE_SETTINGS,
                   [](String v){ userName = v; saveCfg(); });
          return;
        case 2:  // Chat label -> the "You:" tag (name or initials)
          openText("Chat label", youLabel, "your name tag in chat (e.g. initials)", false, MODE_SETTINGS,
                   [](String v){ youLabel = v.length() ? v : String("You"); saveCfg(); });
          return;
        case 3: { // Timezone: cycle -12h..+14h in 1h steps
          tzOffsetMin += 60; if (tzOffsetMin > 14 * 60) tzOffsetMin = -12 * 60; break; }
        case 4: { // Brightness: 20/40/60/80/100%
          int pct = (brightness * 100) / 255; pct += 20; if (pct > 100) pct = 20;
          brightness = (pct * 255) / 100; applyBrightness(); break; }
        case 5: soundsOn    = !soundsOn;    break;
        case 6: trackballOn = !trackballOn; break;
        case 7: webSearchOn = !webSearchOn; break;
        case 8: statusMode = (statusMode + 1) % 3; break;   // cycle IP/Demo/Phone
        case 9: startCalibration(); return;    // Calibrate touch
      }
      break;
    case PG_SYSTEM:
      switch (selIdx) {
        case 1: uiMode = MODE_ABOUT; drawAbout(); return;               // About
        case 2:  // WiFi setup: type SSID, then password, then join
          openText("WiFi network", activeSsid(), "enter the 2.4GHz SSID", false, MODE_SETTINGS,
            [](String ssid){
              openText("WiFi password", "", "for '" + ssid + "'", true, MODE_SETTINGS,
                [ssid](String pass){
                  setMsg = joinWifi(ssid, pass) ? "joined " + ssid : "join failed: " + ssid;
                  setPage = PG_SYSTEM; selIdx = 2;
                });
            });
          return;
        case 3:  // Map key entry (SSH + TCP shell moved to the SSH page)
          openText("Map key (Geoapify)", mapKey, "paste your API key", false, MODE_SETTINGS,
                   [](String v){ mapKey = v; saveCfg(); setPage = PG_SYSTEM; selIdx = 3; });
          return;
        // IP / Uptime rows are read-only status
      }
      break;
    case PG_SSH:
      switch (selIdx) {
        case 1: sshOn = !sshOn; break;                 // SSH server (encrypted, :22)
        case 2: remoteShellOn = !remoteShellOn; break; // TCP shell (plaintext, :23)
        case 3:  // User
          openText("SSH user", sshUser, "login name for ssh", false, MODE_SETTINGS,
                   [](String v){ sshUser = v; saveCfg(); setPage = PG_SSH; selIdx = 3; });
          return;
        case 4:  // Password (cleartext, per request)
          openText("SSH password", sshPass, "login password for ssh", false, MODE_SETTINGS,
                   [](String v){ sshPass = v; saveCfg(); setPage = PG_SSH; selIdx = 4; });
          return;
        // case 5 Connect: read-only helper text
        case 6: sshRegenHostKey(); setMsg = "new host key (clients must re-accept)"; break;
      }
      break;
  }
  saveCfg(); drawSettings();
}

// POST JSON to a URL (secure=true for https). Up to 2 extra headers. Returns the
// response body, or "" with *err set. Client objects stay alive across the POST.
static String httpPostJSON(bool secure, const String& url, const String& body,
                           const char* h1n, const char* h1v,
                           const char* h2n, const char* h2v, String& err) {
  WiFiClientSecure tls; WiFiClient plain;
  HTTPClient http; http.setTimeout(25000);
  bool ok;
  if (secure) { tls.setInsecure(); ok = http.begin(tls, url); }
  else        { ok = http.begin(plain, url); }
  if (!ok) { err = "begin failed"; return ""; }
  http.addHeader("content-type", "application/json");
  if (h1n) http.addHeader(h1n, h1v);
  if (h2n) http.addHeader(h2n, h2v);
  int code = http.POST(body);
  String payload = http.getString();
  http.end();
  if (code <= 0) { err = String("net ") + http.errorToString(code); return ""; }
  return payload;   // caller inspects for API-level errors
}

// Build the system prompt, personalized with the user's name and (if NTP-synced)
// the current local time so the AI is time-aware.
static String buildSysPrompt() {
  String s = SYS_PROMPT;
  if (userName.length()) s += " The user's name is " + userName + "; address them by name.";
  time_t nowt = time(nullptr);
  if (nowt > 1700000000) {                 // NTP has set the clock
    nowt += (time_t)tzOffsetMin * 60;
    struct tm tmv; gmtime_r(&nowt, &tmv);
    char buf[40]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    s += " Current local date/time: "; s += buf; s += ".";
  }
  return s;
}

// Ask the currently-selected AI provider. Returns reply text (or an "[error] ..." string).
static String askAI(const String& prompt) {
  if (WiFi.status() != WL_CONNECTED) return "[error] WiFi not connected";
  String err, p, sp = buildSysPrompt();
  JsonDocument r;

  if (aiProvider == "anthropic") {
    if (!kAnthropic.length()) return "[no Anthropic key — set via /key anthropic <key>]";
    JsonDocument req; req["model"] = aiModel; req["max_tokens"] = 500; req["system"] = sp;
    JsonArray msgs_ = req["messages"].to<JsonArray>();
    { JsonObject m = msgs_.add<JsonObject>(); m["role"] = "user"; m["content"] = prompt; }
    // tools the model may call (each individually toggleable via /tools)
    JsonArray tools = req["tools"].to<JsonArray>();
    if (toolShowMap) { JsonObject t = tools.add<JsonObject>();
      t["name"] = "show_map"; t["description"] = "Display a map on the device screen centered at a latitude/longitude. Use your own knowledge of place coordinates.";
      JsonObject sc = t["input_schema"].to<JsonObject>(); sc["type"] = "object";
      JsonObject pr = sc["properties"].to<JsonObject>();
      pr["lat"]["type"] = "number"; pr["lon"]["type"] = "number";
      JsonArray rq = sc["required"].to<JsonArray>(); rq.add("lat"); rq.add("lon"); }
    if (toolGetLoc) { JsonObject t = tools.add<JsonObject>();
      t["name"] = "get_location"; t["description"] = "Get the device's current GPS latitude/longitude.";
      JsonObject sc = t["input_schema"].to<JsonObject>(); sc["type"] = "object"; sc["properties"].to<JsonObject>(); }
    for (int round = 0; round < 3; round++) {
      String body; serializeJson(req, body);
      p = httpPostJSON(true, "https://api.anthropic.com/v1/messages", body,
                       "x-api-key", kAnthropic.c_str(), "anthropic-version", "2023-06-01", err);
      if (p == "") return "[error] " + err;
      if (deserializeJson(r, p)) return "[bad JSON]";
      if (r["type"] == "error") return String("[api] ") + (const char*)(r["error"]["message"] | "");
      String text;
      JsonObject asst = msgs_.add<JsonObject>(); asst["role"] = "assistant";
      JsonArray ac = asst["content"].to<JsonArray>();
      std::vector<String> tuId, tuRes;
      for (JsonObject b : r["content"].as<JsonArray>()) {
        String type = (const char*)(b["type"] | "");
        JsonObject cc = ac.add<JsonObject>();
        if (type == "text") { cc["type"] = "text"; cc["text"] = (const char*)(b["text"] | ""); text += (const char*)(b["text"] | ""); }
        else if (type == "tool_use") {
          cc["type"] = "tool_use"; cc["id"] = (const char*)b["id"]; cc["name"] = (const char*)b["name"]; cc["input"] = b["input"];
          String name = (const char*)(b["name"] | ""), res;
          if (name == "show_map") { pMapLat = b["input"]["lat"] | 0.0; pMapLon = b["input"]["lon"] | 0.0; pendingMap = true; res = "map displayed on device"; }
          else if (name == "get_location") { res = locValid ? String(locLat, 5) + "," + String(locLon, 5) : "no GPS fix"; }
          else res = "unknown tool";
          tuId.push_back((const char*)(b["id"] | "")); tuRes.push_back(res);
        }
      }
      if (tuId.empty()) return text.length() ? text : "[no text]";   // done, no tool call
      JsonObject ur = msgs_.add<JsonObject>(); ur["role"] = "user";
      JsonArray urc = ur["content"].to<JsonArray>();
      for (size_t i = 0; i < tuId.size(); i++) {
        JsonObject tr = urc.add<JsonObject>(); tr["type"] = "tool_result"; tr["tool_use_id"] = tuId[i]; tr["content"] = tuRes[i];
      }
    }
    return "[tool loop limit]";
  }
  if (aiProvider == "openai") {
    if (!kOpenAI.length()) return "[no OpenAI key — set via /key openai <key>]";
    JsonDocument req; req["model"] = aiModel; req["max_tokens"] = 400;
    JsonArray a = req["messages"].to<JsonArray>();
    JsonObject s = a.add<JsonObject>(); s["role"] = "system"; s["content"] = sp;
    JsonObject u = a.add<JsonObject>(); u["role"] = "user"; u["content"] = prompt;
    String body; serializeJson(req, body);
    String auth = String("Bearer ") + kOpenAI;
    p = httpPostJSON(true, "https://api.openai.com/v1/chat/completions", body,
                     "authorization", auth.c_str(), nullptr, nullptr, err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    if (r["error"]) return String("[api] ") + (const char*)(r["error"]["message"] | "");
    return String((const char*)(r["choices"][0]["message"]["content"] | "[no text]"));
  }
  if (aiProvider == "gemini") {
    if (!kGemini.length()) return "[no Gemini key — set via /key gemini <key>]";
    JsonDocument req;
    req["systemInstruction"]["parts"][0]["text"] = sp;
    JsonObject u = req["contents"].to<JsonArray>().add<JsonObject>();
    u["role"] = "user"; u["parts"][0]["text"] = prompt;
    req["generationConfig"]["maxOutputTokens"] = 400;
    String body; serializeJson(req, body);
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
                 aiModel + ":generateContent?key=" + kGemini;
    p = httpPostJSON(true, url, body, nullptr, nullptr, nullptr, nullptr, err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    if (r["error"]) return String("[api] ") + (const char*)(r["error"]["message"] | "");
    return String((const char*)(r["candidates"][0]["content"]["parts"][0]["text"] | "[no text]"));
  }
  if (aiProvider == "ollama") {
    if (!ollamaHost.length()) return "[set Ollama host via /ollama <host:port>]";
    JsonDocument req; req["model"] = aiModel; req["stream"] = false;
    JsonArray a = req["messages"].to<JsonArray>();
    JsonObject s = a.add<JsonObject>(); s["role"] = "system"; s["content"] = sp;
    JsonObject u = a.add<JsonObject>(); u["role"] = "user"; u["content"] = prompt;
    String body; serializeJson(req, body);
    p = httpPostJSON(false, String("http://") + ollamaHost + "/api/chat", body,
                     nullptr, nullptr, nullptr, nullptr, err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    return String((const char*)(r["message"]["content"] | "[no text]"));
  }
  return "[unknown provider: " + aiProvider + "]";
}

static String aiLabel() {
  if (aiProvider == "openai") return "GPT";
  if (aiProvider == "gemini") return "Gemini";
  if (aiProvider == "ollama") return "Ollama";
  return "Haiku";
}

// Light markdown stripper for the tiny screen: drop **bold**/__/` markers and
// leading # header hashes so they don't render as literal clutter. Not a renderer.
static String deMarkdown(String s) {
  s.replace("**", ""); s.replace("__", ""); s.replace("`", "");
  int i = 0;
  while ((i = s.indexOf('#', i)) >= 0) {
    if (i == 0 || s[i - 1] == '\n') {           // only header hashes at line start
      int j = i; while (j < (int)s.length() && (s[j] == '#' || s[j] == ' ')) j++;
      s.remove(i, j - i);
    } else i++;
  }
  return s;
}

static void sendPrompt(const String& prompt) {
  scrollLines = 0;
  String lbl = aiLabel() + ": ";
  addMsg(youLabel + ": " + prompt, userColor);
  addMsg("...", C_DIM); draw();
  String reply = deMarkdown(askAI(prompt));
  Serial.println(lbl + reply);
  if (!msgs.empty()) msgs.pop_back();       // remove "..."
  addMsg(lbl + reply, aiColor);
  // show YOUR message at the top of the new exchange (scroll down for long replies)
  setFont(chatFontIdx);
  std::vector<String> ex;
  wrapMsg(youLabel + ": " + prompt, scrW - 4, ex);
  wrapMsg(lbl + reply, scrW - 4, ex);
  scrollLines = (int)ex.size() > lastRows ? (int)ex.size() - lastRows : 0;
  draw();
  if (pendingMap) { pendingMap = false; showMap(pMapLat, pMapLon, 13); }   // AI asked for a map
}

static uint8_t readKey() {
  Wire.requestFrom(KB_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0;
}

// ---- GT911 capacitive touch ----
static void gtProbe() {
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { gtAddr = a; break; }
  }
  Serial.printf("[touch] GT911 addr=0x%02X\n", gtAddr);
}
// Read a GT911 16-bit register block using a repeated-START (no STOP between the
// address write and the read) — required for correct byte alignment.
static bool gtRead(uint16_t reg, uint8_t* out, uint8_t n) {
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  uint8_t got = Wire.requestFrom(gtAddr, n);
  for (uint8_t i = 0; i < n && Wire.available(); i++) out[i] = Wire.read();
  return got == n;
}
static void gtWrite(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(gtAddr);
  Wire.write((uint8_t)(reg >> 8)); Wire.write((uint8_t)(reg & 0xFF)); Wire.write(val);
  Wire.endTransmission();
}
// Read one touch point. Returns raw controller coords in rx/ry; false if no touch.
static bool gtReadRaw(int& rx, int& ry) {
  if (!gtAddr) return false;
  uint8_t st = 0;
  if (!gtRead(0x814E, &st, 1)) return false;    // GT_POINT_INFO
  bool ready = st & 0x80, touched = false;
  if (ready && (st & 0x0F)) {
    uint8_t p[6];
    if (gtRead(0x8150, p, 6)) {                 // point 1 coords: xL,xH,yL,yH,szL,szH
      rx = p[0] | (p[1] << 8); ry = p[2] | (p[3] << 8); touched = true;
    }
  }
  if (ready) gtWrite(0x814E, 0);                // clear buffer-status flag
  return touched;
}
// Map raw GT911 coords -> screen coords via the calibrated affine transform.
// Uncalibrated defaults are identity (a runtime calibration wizard sets these).
static void gtMap(int rx, int ry, int& sx, int& sy) {
  sx = (int)(tcAx * rx + tcBx * ry + tcCx);
  sy = (int)(tcAy * rx + tcBy * ry + tcCy);
  if (sx < 0) sx = 0; if (sx >= scrW) sx = scrW - 1;
  if (sy < 0) sy = 0; if (sy >= scrH) sy = scrH - 1;
}

// ---- 3-point touch calibration wizard (MODE_CALIB) ----
// Solve s = a*rx + b*ry + c from three (rx,ry)->s samples (Cramer's rule).
static bool solve3(const float rx[3], const float ry[3], const float s[3],
                   float& a, float& b, float& c) {
  float det = rx[0]*(ry[1]-ry[2]) - ry[0]*(rx[1]-rx[2]) + (rx[1]*ry[2]-rx[2]*ry[1]);
  if (fabsf(det) < 1e-3f) return false;
  float da = s[0]*(ry[1]-ry[2]) - ry[0]*(s[1]-s[2]) + (s[1]*ry[2]-s[2]*ry[1]);
  float db = rx[0]*(s[1]-s[2]) - s[0]*(rx[1]-rx[2]) + (rx[1]*s[2]-rx[2]*s[1]);
  float dc = rx[0]*(ry[1]*s[2]-s[1]*ry[2]) - ry[0]*(rx[1]*s[2]-s[1]*rx[2]) + s[0]*(rx[1]*ry[2]-rx[2]*ry[1]);
  a = da/det; b = db/det; c = dc/det; return true;
}
static int   calStep = 0;
static long  calAccX = 0, calAccY = 0; static int calAccN = 0;   // press accumulator
static float calRX[3], calRY[3], calTX[3], calTY[3];
static void drawCalTarget() {
  tft.fillScreen(C_BG); tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_INK, C_BG);
  tft.drawString("Touch calibration", scrW/2, 26);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("tap each target (" + String(calStep+1) + "/3)", scrW/2, 44);
  int x = (int)calTX[calStep], y = (int)calTY[calStep];
  tft.drawCircle(x, y, 10, C_AMBER); tft.drawCircle(x, y, 2, C_AMBER);
  tft.drawFastHLine(x-14, y, 28, C_TEAL); tft.drawFastVLine(x, y-14, 28, C_TEAL);
}
static void startCalibration() {
  uiMode = MODE_CALIB; calStep = 0; calAccX = calAccY = 0; calAccN = 0;
  calTX[0] = 30;        calTY[0] = 30;
  calTX[1] = scrW - 30; calTY[1] = 30;
  calTX[2] = 30;        calTY[2] = scrH - 30;
  drawCalTarget();
}
static void calRecord(int rx, int ry) {   // called on a discrete tap during MODE_CALIB
  calRX[calStep] = rx; calRY[calStep] = ry; calStep++;
  if (calStep < 3) { drawCalTarget(); return; }
  float ax,bx,cx,ay,by,cy;
  bool ok = solve3(calRX, calRY, calTX, ax, bx, cx) &&
            solve3(calRX, calRY, calTY, ay, by, cy);
  if (ok) { tcAx=ax; tcBx=bx; tcCx=cx; tcAy=ay; tcBy=by; tcCy=cy;
            touchCalValid = true; saveCfg();
            Serial.printf("[cal] ok ax=%.4f bx=%.4f cx=%.1f ay=%.4f by=%.4f cy=%.1f\n", ax,bx,cx,ay,by,cy);
            uiMode = MODE_CHAT; draw(); }
  else { Serial.println("[cal] degenerate - restarting"); calStep = 0; drawCalTarget(); }   // retry
}

// Active credentials: on-device override (Settings) wins over secrets.h defaults.
static String activeSsid() { return wifiSsid.length() ? wifiSsid : String(DEFAULT_WIFI_SSID); }
static String activePass() { return wifiSsid.length() ? wifiPass : String(DEFAULT_WIFI_PASS); }

static void connectWifi() {
  String ssid = activeSsid();
  if (ssid.isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // disable modem sleep so inbound TCP (the shell) is reliable
  WiFi.begin(ssid.c_str(), activePass().c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
}

// Save + (re)connect to a network entered on-device. Returns true if associated.
static bool joinWifi(const String& ssid, const String& pass) {
  wifiSsid = ssid; wifiPass = pass; saveCfg();
  WiFi.disconnect(); WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// Branded boot splash: RoostOS wordmark + a little chick perched on an antenna.
// Brief "charging connected" splash (shown when the pack voltage rises on plug-in).
static void drawChargeSplash() {
  tft.fillScreen(C_BG); tft.setTextDatum(MC_DATUM);
  int w = 128, h = 58, x = (scrW - w) / 2, y = (scrH - h) / 2 - 8;
  tft.drawRoundRect(x, y, w, h, 6, C_TEAL); tft.fillRect(x + w, y + h / 2 - 8, 6, 16, C_TEAL);   // battery + nub
  int pct = batteryPct(); tft.fillRect(x + 3, y + 3, (w - 6) * pct / 100, h - 6, C_TEAL);
  // lightning bolt (amber)
  int cx = scrW / 2, cy = y + h / 2;
  tft.fillTriangle(cx + 4, cy - 16, cx - 8, cy + 3, cx + 2, cy + 3, C_AMBER);
  tft.fillTriangle(cx - 4, cy + 16, cx + 8, cy - 3, cx - 2, cy - 3, C_AMBER);
  tft.setFreeFont(&FreeSans12pt7b); tft.setTextColor(C_AMBER, C_BG);
  tft.drawString("Charging  " + String(pct) + "%", scrW / 2, y + h + 22);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
}
static void drawSplash() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSans18pt7b);
  tft.setTextColor(C_TEAL, C_BG);   tft.drawString("Roost", 40, 78);
  int w = tft.textWidth("Roost");
  tft.setTextColor(C_INDIGO, C_BG); tft.drawString("OS", 40 + w, 78);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);    tft.drawString("C O M M U N I C A T O R", 42, 116);
  // antenna + chick
  int ax = 250, aBase = 158, aTip = 46;
  tft.drawFastVLine(ax, aTip, aBase - aTip, C_DIM);
  tft.drawFastVLine(ax + 1, aTip, aBase - aTip, C_DIM);
  tft.fillCircle(ax, aTip, 3, C_AMBER);
  int cx = ax + 14, cy = aTip + 10;
  tft.fillCircle(cx, cy + 8, 11, C_AMBER);
  tft.fillCircle(cx + 6, cy, 8, C_AMBER);
  tft.fillTriangle(cx + 13, cy - 2, cx + 13, cy + 4, cx + 22, cy + 1, C_TEAL);
  tft.fillCircle(cx + 8, cy - 2, 2, C_BG);
  tft.drawLine(cx, cy + 18, ax, aTip + 2, C_DIM);
  tft.setTextColor(C_DIM, C_BG); tft.drawString(ROOST_COMM_VERSION, 40, 206);
}

void setup() {
  pinMode(PIN_POWERON, OUTPUT); digitalWrite(PIN_POWERON, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  pinMode(TB_UP, INPUT_PULLUP); pinMode(TB_DOWN, INPUT_PULLUP); pinMode(TB_CLICK, INPUT_PULLUP);
  pinMode(1, INPUT_PULLUP); pinMode(2, INPUT_PULLUP);   // trackball LEFT/RIGHT (were floating -> spurious turns)
  Serial.begin(115200); delay(300);
  Serial.printf("\n=== RoostOS Communicator APP %s ===\n", ROOST_COMM_VERSION);
  Wire.begin(I2C_SDA, I2C_SCL);
  gtProbe();   // detect GT911 touch controller
  GPSser.begin(9600, SERIAL_8N1, 43, 44);   // L76K GPS on UART1
  TJpgDec.setJpgScale(1); TJpgDec.setSwapBytes(true); TJpgDec.setCallback(jpgToTft);

  tft.init(); tft.setRotation(1);
  scrW = tft.width(); scrH = tft.height();
  C_BG     = tft.color565(0x0d, 0x11, 0x17);
  C_PANEL  = tft.color565(0x15, 0x1d, 0x2c);
  C_INK    = tft.color565(0xee, 0xf2, 0xfb);
  C_DIM    = tft.color565(0x93, 0xa0, 0xc4);
  C_TEAL   = tft.color565(0x34, 0xe2, 0xc0);
  C_INDIGO = tft.color565(0x7c, 0x8c, 0xff);
  C_AMBER  = tft.color565(0xff, 0xbe, 0x4d);
  userColor = C_INDIGO; aiColor = C_TEAL; accentColor = C_AMBER;   // defaults
  loadCfg();                                 // override from saved settings (NVS)
  applyBrightness();                         // LEDC PWM backlight at saved level

  drawSplash();
  delay(splashMs);   // adjustable in settings

  int save = chatFontIdx; chatFontIdx = 0;   // boot in tiny so startup lines fit
  addMsg("Booting... connecting WiFi", C_DIM); draw();
  connectWifi();
  addMsg(WiFi.status() == WL_CONNECTED
    ? String("Ready to Roost! Type a message + Enter, or tap the menu.")
    : String("WiFi failed - check SSS-FAMILY"), C_TEAL);
  chatFontIdx = save;                         // swap to preferred size
  if (WiFi.status() == WL_CONNECTED)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; tz applied in buildSysPrompt
  draw();
  Serial.printf("[wifi] status=%d ip=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  Serial.println("cmds: ask <t> | font <n> | inputfont <n> | color user|ai <name> | ip");
}

static String serialBuf;
static bool tbUpPrev = true, tbDnPrev = true;
static uint32_t lastScroll = 0;

static int fontArgToIdx(const String& arg, int cur) {
  if (arg == "tiny") return 0; if (arg == "small") return 1;
  if (arg == "medium") return 2; if (arg == "large") return 3;
  int pt = arg.toInt(); if (pt == 0) return cur;
  return pt <= 6 ? 0 : (pt <= 10 ? 1 : (pt <= 15 ? 2 : 3));
}

// Is the Ollama host answering? (quick GET /api/tags with short timeouts)
static bool ollamaReachable() {
  if (!ollamaHost.length()) return false;
  WiFiClient c; HTTPClient h;
  h.setConnectTimeout(3000); h.setTimeout(4000);
  if (!h.begin(c, String("http://") + ollamaHost + "/api/tags")) return false;
  int code = h.GET(); h.end();
  return code == 200;
}
// Verify-then-switch: only change the active provider if it's configured and (for
// local Ollama) actually reachable. Returns success; sets msg for display.
static bool switchProvider(const String& p, String& msg) {
  bool valid = false; for (int i = 0; i < NPROV; i++) if (p == PROVIDERS[i]) valid = true;
  if (!valid) { msg = "unknown provider"; return false; }
  if (!providerConfigured(p)) { msg = "no key for " + p; return false; }
  if (p == "ollama" && WiFi.status() == WL_CONNECTED && !ollamaReachable()) {
    msg = "ollama unreachable @ " + ollamaHost; return false;
  }
  aiProvider = p; aiModel = defaultModel(p); saveCfg();
  msg = "provider: " + aiProvider + " / " + aiModel; return true;
}

// command matches full name OR a >=3-char prefix (so /fon, /set, /col, /scr, /spl, /inp work)
static bool cmdIs(const String& tok, const char* full) {
  String f = full;
  return tok == f || (tok.length() >= 3 && f.startsWith(tok));
}
// Geocode a place/zip/address to lat,lon via Geoapify (needs the map key + WiFi).
static bool geocodePlace(const String& q, double& lat, double& lon) {
  if (!mapKey.length() || WiFi.status() != WL_CONNECTED) return false;
  String enc; for (size_t i = 0; i < q.length(); i++) { char c = q[i]; enc += (c == ' ') ? String("%20") : String(c); }
  String url = "https://api.geoapify.com/v1/geocode/search?limit=1&text=" + enc + "&apiKey=" + mapKey;
  WiFiClientSecure tls; tls.setInsecure(); HTTPClient h; h.setTimeout(10000);
  if (!h.begin(tls, url)) return false;
  int code = h.GET(); if (code != 200) { h.end(); return false; }
  String body = h.getString(); h.end();
  JsonDocument d; if (deserializeJson(d, body)) return false;
  JsonArray f = d["features"].as<JsonArray>();
  if (f.size() == 0) return false;
  lon = f[0]["geometry"]["coordinates"][0].as<double>();
  lat = f[0]["geometry"]["coordinates"][1].as<double>();
  return true;
}
// Web search via the DuckDuckGo Instant Answer API (no key). Returns snippet text.
static String ddgSearch(const String& q) {
  if (WiFi.status() != WL_CONNECTED) return "";
  String enc; for (size_t i = 0; i < q.length(); i++) { char c = q[i]; enc += (c == ' ') ? String("+") : String(c); }
  // DDG's HTML results endpoint returns real result snippets (the Instant Answer
  // API is only definitions). Read a capped chunk of the page and scrape snippets.
  String url = "https://html.duckduckgo.com/html/?q=" + enc;
  WiFiClientSecure tls; tls.setInsecure(); HTTPClient h; h.setTimeout(12000);
  if (!h.begin(tls, url)) return "";
  h.setUserAgent("Mozilla/5.0 (RoostOS)");
  const char* hdrs[] = {"Location"}; h.collectHeaders(hdrs, 1);
  int code = h.GET();
  if (code != 200) { h.end(); return ""; }
  WiFiClient* st = h.getStreamPtr();
  String body; body.reserve(50000); uint32_t t0 = millis();
  while (h.connected() && body.length() < 48000 && millis() - t0 < 12000) {
    while (st->available() && body.length() < 48000) body += (char)st->read();
    if (!st->available()) delay(4);
  }
  h.end();
  String out; int idx = 0, n = 0;
  while (n < 5 && (idx = body.indexOf("result__snippet", idx)) >= 0) {
    int gt = body.indexOf('>', idx); if (gt < 0) break;
    int end = body.indexOf("</a>", gt); if (end < 0) break;
    String seg = body.substring(gt + 1, end), txt; bool tag = false;
    for (size_t i = 0; i < seg.length(); i++) { char c = seg[i]; if (c == '<') tag = true; else if (c == '>') tag = false; else if (!tag) txt += c; }
    txt.replace("&#x27;", "'"); txt.replace("&amp;", "&"); txt.replace("&quot;", "\""); txt.replace("&#x2F;", "/"); txt.trim();
    if (txt.length() > 8) { out += "- " + txt + "\n"; n++; }
    idx = end;
  }
  return out;
}
// Apply a config command (from serial or an on-device "/..." message). Returns a
// short status string for display; "" if the command was not recognized.
static String applyCfgCmd(String s) {
  s.trim();
  int sp = s.indexOf(' ');
  String tok = (sp < 0 ? s : s.substring(0, sp)); tok.toLowerCase();
  String rest = (sp < 0 ? String("") : s.substring(sp + 1)); rest.trim();
  if (cmdIs(tok, "settings")) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); return " "; }
  if (cmdIs(tok, "font")) { chatFontIdx = fontArgToIdx(rest, chatFontIdx); saveCfg(); draw(); return String("chat font: ") + FONTS[chatFontIdx].name; }
  if (cmdIs(tok, "inputfont")) { inputFontIdx = fontArgToIdx(rest, inputFontIdx); saveCfg(); draw(); return String("input font: ") + FONTS[inputFontIdx].name; }
  if (cmdIs(tok, "color")) {
    int p = rest.indexOf(' ');
    if (p < 0) return "usage: /color user|ai|accent <name>";
    String who = rest.substring(0, p), name = rest.substring(p + 1); name.trim();
    uint16_t col = namedColor(name);
    if (col == 0xFFFF) return "colors: teal indigo amber red green cyan magenta orange purple sky lime rose gold mint ink";
    if (who.startsWith("u")) userColor = col; else if (who.startsWith("ai") || who == "a") aiColor = col;
    else if (who.startsWith("acc")) accentColor = col; else return "usage: /color user|ai|accent <name>";
    saveCfg(); draw(); return who + " color set";
  }
  if (cmdIs(tok, "you")) { youLabel = rest.length() ? rest : String("You"); saveCfg(); if (uiMode == MODE_CHAT) draw(); return "chat label: " + youLabel; }
  if (cmdIs(tok, "scroll")) { scrollStep = constrain(rest.toInt(), 1, 20); saveCfg(); return String("scroll rate: ") + scrollStep; }
  if (cmdIs(tok, "splash")) { splashMs = constrain(rest.toInt(), 0, 15000); saveCfg(); return String("splash: ") + splashMs + "ms"; }
  if (cmdIs(tok, "provider")) {
    String v = rest; v.toLowerCase();
    if (v.isEmpty()) {
      String s = "providers (provider <name>):";
      for (int i = 0; i < NPROV; i++) { String p = PROVIDERS[i];
        s += String("\r\n  ") + (p == aiProvider ? "* " : "  ") + p + " - " + (providerConfigured(p) ? "ready" : "no key"); }
      return s;
    }
    String m; switchProvider(v, m); return m;
  }
  if (cmdIs(tok, "model")) {
    if (rest.length()) { aiModel = rest; saveCfg(); return String("model: ") + aiModel; }
    String opts = aiProvider == "anthropic" ? "claude-haiku-4-5, claude-sonnet-4-6"
                : aiProvider == "openai"    ? "gpt-4o-mini, gpt-4o"
                : aiProvider == "gemini"    ? "gemini-flash-lite-latest, gemini-2.5-flash"
                                            : "llama3.2, qwen2.5, phi3";
    return "model: " + aiModel + "\r\n  " + aiProvider + " options: " + opts + "  (or model <any>)";
  }
  // --- device / personalization ---
  if (cmdIs(tok, "name")) { userName = rest; saveCfg(); return String("name: ") + (userName.length() ? userName : "(cleared)"); }
  if (cmdIs(tok, "tz") || cmdIs(tok, "timezone")) {
    if (rest.length()) { tzOffsetMin = constrain((int)(rest.toFloat() * 60), -12 * 60, 14 * 60); saveCfg(); }
    return String("timezone: UTC") + (tzOffsetMin >= 0 ? "+" : "") + String(tzOffsetMin / 60.0, 1) + "h";
  }
  if (cmdIs(tok, "brightness")) { if (rest.length()) { brightness = constrain((rest.toInt() * 255) / 100, 8, 255); applyBrightness(); saveCfg(); } return String("brightness: ") + String((brightness * 100) / 255) + "%"; }
  if (cmdIs(tok, "sounds")) { soundsOn = (rest != "off" && rest != "0"); saveCfg(); return String("sounds: ") + (soundsOn ? "on" : "off"); }
  if (cmdIs(tok, "trackball")) { trackballOn = (rest != "off" && rest != "0"); saveCfg(); return String("trackball: ") + (trackballOn ? "on" : "off"); }
  if (cmdIs(tok, "websearch")) { webSearchOn = (rest == "on" || rest == "1"); saveCfg(); return String("web search: ") + (webSearchOn ? "on" : "off"); }
  if (cmdIs(tok, "shell")) { remoteShellOn = (rest == "on" || rest == "1"); saveCfg(); return String("remote shell: ") + (remoteShellOn ? "on (port 23)" : "off"); }
  if (cmdIs(tok, "status") || cmdIs(tok, "demo")) {   // status bar mode
    String v = rest; v.toLowerCase();
    if (v.startsWith("ip") || v == "off") statusMode = STAT_IP;
    else if (v.startsWith("demo") || v == "on") statusMode = STAT_DEMO;
    else if (v.startsWith("phone")) statusMode = STAT_PHONE;
    else return "status: ip | demo | phone";
    saveCfg(); if (uiMode == MODE_CHAT) draw(); return String("status bar: ") + statusName();
  }
  if (cmdIs(tok, "prompt")) {
    String v = rest; v.trim();
    if (v == "empty" || v == "none") promptWord = "";
    else if (v.length()) promptWord = v;          // roostos | ai | os | custom
    saveCfg();
    return String("prompt: ") + (promptWord.length() ? promptWord : "(empty)") + "> ";
  }
  if (cmdIs(tok, "ssh")) { sshOn = (rest == "on" || rest == "1"); saveCfg();
    return String("ssh: ") + (sshOn ? "on - ssh " + sshUser + "@<ip> (pw set in /sshpass)" : "off"); }
  if (cmdIs(tok, "sshuser")) { if (rest.length()) { sshUser = rest; saveCfg(); } return "ssh user: " + sshUser; }
  if (cmdIs(tok, "sshpass")) { if (rest.length()) { sshPass = rest; saveCfg(); } return String("ssh password: set (") + sshPass.length() + " chars)"; }
  if (tok == "gps") {
    if (locValid) return "gps: " + String(locLat, 5) + "," + String(locLon, 5) + "  sats:" + String(gps.satellites.value());
    return "gps: no fix yet (sats:" + String(gps.satellites.value()) + ")";
  }
  if (tok == "map") {        // exact-match (so it doesn't collide with 'mapkey')
    double la, lo; String r = rest; r.trim();
    if (r == "home") { if (!homeValid) return "no home set - use: home <lat lon> or home <place>"; la = homeLat; lo = homeLon; }
    else if (r.length()) {
      int q = r.indexOf(' ');
      if (q > 0 && (isdigit((unsigned char)r[0]) || r[0] == '-')) { la = r.substring(0, q).toFloat(); lo = r.substring(q + 1).toFloat(); }
      else if (!geocodePlace(r, la, lo)) return "couldn't find: " + r;   // place / zip / address
    }
    else if (locValid)     { la = locLat; lo = locLon; }        // 1) live GPS
    else if (homeValid)    { la = homeLat; lo = homeLon; }       // 2) home
    else if (lastMapValid) { la = lastMapLat; lo = lastMapLon; } // 3) last saved map
    else return "no location - set home <lat lon> | home <place>, or map <lat lon>";
    showMap(la, lo, 14); return "map";
  }
  if (tok == "home") {
    String r = rest; r.trim();
    if (r.length()) {
      double la, lo; int q = r.indexOf(' ');
      if (q > 0 && (isdigit((unsigned char)r[0]) || r[0] == '-'))
        { la = r.substring(0, q).toFloat(); lo = r.substring(q + 1).toFloat(); }
      else if (!geocodePlace(r, la, lo)) return "couldn't find: " + r;
      homeLat = la; homeLon = lo; homeValid = true; saveCfg();
      return "home set: " + String(homeLat, 5) + "," + String(homeLon, 5);
    }
    return homeValid ? ("home: " + String(homeLat, 5) + "," + String(homeLon, 5))
                     : "home not set - use: home <lat lon> | home <place/zip>";
  }
  if (tok == "game") { String g = rest; g.toLowerCase();
    if (g.startsWith("sn")) { gameLaunch(0); return "snake"; }
    if (g.startsWith("su")) { gameLaunch(1); return "sudoku"; }
    return "game: snake | sudoku"; }
  if (tok == "snake")  { gameLaunch(0); return "snake"; }
  if (tok == "sudoku") { gameLaunch(1); return "sudoku"; }
  if (tok == "slide")  { gameLaunch(2); return "slide"; }
  if (cmdIs(tok, "calibrate")) { startCalibration(); return "calibrate: tap the 3 targets on the screen"; }
  if (cmdIs(tok, "clear")) { clearChat(); return "chat cleared"; }
  if (cmdIs(tok, "mapkey")) { if (rest.length()) { mapKey = rest; saveCfg(); } return String("map key: ") + (mapKey.length() ? "set" : "none"); }
  if (cmdIs(tok, "key")) {   // /key anthropic|openai|gemini <api-key>
    int p = rest.indexOf(' ');
    if (p < 0) return "usage: key <anthropic|openai|gemini> <api-key>";
    String which = rest.substring(0, p); which.toLowerCase();
    String val = rest.substring(p + 1); val.trim();
    if      (which.startsWith("a")) kAnthropic = val;
    else if (which.startsWith("o")) kOpenAI = val;
    else if (which.startsWith("g")) kGemini = val;
    else return "provider: anthropic|openai|gemini";
    saveCfg();
    return which + " key: " + (val.length() ? "set (" + String(val.length()) + " chars)" : "cleared");
  }
  if (cmdIs(tok, "ollama")) { if (rest.length()) { ollamaHost = rest; saveCfg(); } return String("ollama host: ") + (ollamaHost.length() ? ollamaHost : "none"); }
  if (cmdIs(tok, "wifi")) {
    int p = rest.indexOf(' ');
    if (p < 0) return "usage: wifi <ssid> <password>";
    String ssid = rest.substring(0, p), pass = rest.substring(p + 1); pass.trim();
    return joinWifi(ssid, pass) ? "joined " + ssid : "join failed: " + ssid;
  }
  return "";
}

// ============================================================================
//  GPS (L76K on UART1, pins RX=43 TX=44 @9600) + on-screen maps (Geoapify
//  static JPEG rendered via TJpg_Decoder).
// ============================================================================
static void pollGps() {
  while (GPSser.available()) gps.encode(GPSser.read());
  if (gps.location.isValid() && gps.location.age() < 5000) {
    locLat = gps.location.lat(); locLon = gps.location.lng(); locValid = true;
  }
}
static void mapMsg(const String& m) {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.setTextColor(C_INK, C_BG); tft.drawString(m, 8, 70);
  tft.setTextColor(C_DIM, C_BG); tft.drawString("any key = back to chat", 8, scrH - 12);
}
static void showMap(double lat, double lon, int zoom) {
  uiMode = MODE_MAP;
  if (!mapKey.length())            { mapMsg("No map key. Set one:  /mapkey <key>"); return; }
  if (WiFi.status() != WL_CONNECTED){ mapMsg("Map needs WiFi."); return; }
  mapMsg("Loading map...");
  char clat[16], clon[16]; dtostrf(lat, 0, 6, clat); dtostrf(lon, 0, 6, clon);
  String url = String("https://maps.geoapify.com/v1/staticmap?style=osm-bright"
                      "&width=320&height=224&center=lonlat:") + clon + "," + clat +
               "&zoom=" + zoom + "&format=jpeg&marker=lonlat:" + clon + "," + clat +
               ";color:%2334e2c0;size:medium&apiKey=" + mapKey;
  WiFiClientSecure tls; tls.setInsecure();
  HTTPClient http; http.setTimeout(15000);
  if (!http.begin(tls, url)) { mapMsg("map: begin failed"); return; }
  http.setUserAgent("RoostOS-Communicator");
  int code = http.GET();
  Serial.printf("[map] http=%d keylen=%d\n", code, (int)mapKey.length());   // key intentionally not logged
  if (code != 200) {
    String eb = http.getString(); http.end();
    Serial.printf("[map] err body: %s\n", eb.substring(0, 160).c_str());
    mapMsg("map http " + String(code) + " (see serial)"); return;
  }
  int len = http.getSize();
  size_t cap = (len > 0) ? (size_t)len + 16 : 220000;
  uint8_t* buf = (uint8_t*)ps_malloc(cap);
  if (!buf) { http.end(); mapMsg("map: out of PSRAM"); return; }
  WiFiClient* st = http.getStreamPtr(); size_t got = 0; uint32_t t0 = millis();
  while (http.connected() && (len < 0 || got < (size_t)len) && got < cap && millis() - t0 < 15000) {
    size_t avail = st->available();
    if (avail) { int r = st->readBytes(buf + got, min(avail, cap - got)); if (r > 0) { got += r; t0 = millis(); } }
    else delay(5);
  }
  http.end();
  tft.fillScreen(C_BG);
  TJpgDec.drawJpg(0, 8, buf, got);
  free(buf);
  // remember this location so /map can reopen it without a fix
  lastMapLat = lat; lastMapLon = lon; lastMapValid = true; saveCfg();
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_AMBER, C_BG);
  tft.drawString(String(clat) + "," + clon + (locValid ? " (gps)" : ""), 4, scrH - 11);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("key/click/tap = close", scrW - 118, scrH - 11);
}

// ============================================================================
//  Remote terminal shell (TCP :23) — "SSH-style" chat + config from a computer.
//  `nc <device-ip> 23` (or telnet): just type to chat with the AI (shares the
//  on-screen chat buffer), or use /help /set /get /quit. Gated by remoteShellOn.
//  (Plain TCP now; real SSH via libssh_esp32 is the next transport step.)
// ============================================================================
// ---- real SSH server (src/app/ssh_server.cpp), driven from this shell ----
void sshServerStart(); void sshServerStop(); void sshRegenHostKey();
bool sshPopLine(String& out); void sshQueueOut(const char* s);
bool sshActive(); bool sshIsRunning();
extern "C" void sshSetCreds(const char* u, const char* p);
struct SshPrint : public Print {
  size_t write(uint8_t b) override { char s[2] = {(char)b, 0}; sshQueueOut(s); return 1; }
  size_t write(const uint8_t* buf, size_t n) override {
    String s; s.reserve(n + 1); for (size_t i = 0; i < n; i++) s += (char)buf[i];
    sshQueueOut(s.c_str()); return n; }
};
static SshPrint sshSink;

static WiFiServer shellServer(23);
static WiFiClient shellClient;
static bool   shellStarted = false;
static String shellBuf;
static int    wizStep = -1;        // -1 = not in the /set wizard
static bool   shellLastCR = false;

static const char* WIZ_Q[] = {
  "Your name (blank = skip): ",
  "Timezone, hours from UTC e.g. -5 (blank = skip): ",
  "Anthropic API key - paste sk-ant-... (blank = skip): ",
  "OpenAI API key (blank = skip): ",
  "Gemini API key (blank = skip): ",
  "Ollama host host:port (blank = skip): ",
  "AI provider [anthropic|openai|gemini|ollama] (blank = skip): ",
  "Geoapify map key (blank = skip): ",
  "Brightness %, 10-100 (blank = skip): ",
  "WiFi as 'ssid password' (blank = skip): ",
};
static const char* WIZ_CMD[] = {
  "name", "tz", "key anthropic", "key openai", "key gemini", "ollama",
  "provider", "mapkey", "brightness", "wifi"
};
static const int WIZ_N = 10;

// The shell can drive either the TCP client OR the USB-C serial console.
static Print* shellOut = nullptr;
static void shellPrint(const String& s) { if (shellOut) shellOut->print(s); }
static void shellPrompt() { shellPrint("\r\n" + promptWord + (promptWord.length() ? "> " : "> ")); }
static void shellBanner() {
  shellPrint(String("\r\n=== RoostOS Communicator ") + ROOST_COMM_VERSION + " ===\r\n");
  shellPrint("A handheld AI communicator. You're connected over the network.\r\n");
  shellPrint("Just type to chat. Commands: /help  /set  /get  /quit\r\n");
}
static String shellConfigSummary() {
  String s = "provider: " + aiProvider + " / " + aiModel + "\r\n";
  s += "name: " + (userName.length() ? userName : String("(unset)")) + "\r\n";
  s += "tz: UTC" + String(tzOffsetMin >= 0 ? "+" : "") + String(tzOffsetMin / 60.0, 1) + "h\r\n";
  s += "wifi: " + (WiFi.isConnected() ? dispSsid() : String("down")) + "  ip: " + WiFi.localIP().toString() + "\r\n";
  s += "brightness: " + String((brightness * 100) / 255) + "%  sounds: " + (soundsOn ? "on" : "off") +
       "  trackball: " + (trackballOn ? "on" : "off") + "\r\n";
  s += "map key: " + String(mapKey.length() ? "set" : "none") + "  web search: " + (webSearchOn ? "on" : "off") + "\r\n";
  return s;
}
static String lastPrompt;   // for /retry
static bool    gpsStream = false; static uint32_t gpsStreamT = 0; static Print* streamOut = nullptr;
static String gpsLine() {
  String s = "gps: sats=" + String(gps.satellites.value());
  if (locValid) {
    s = "gps: " + String(locLat, 6) + "," + String(locLon, 6) +
        "  sats=" + String(gps.satellites.value()) +
        "  alt=" + String(gps.altitude.meters(), 0) + "m" +
        "  spd=" + String(gps.speed.kmph(), 1) + "km/h" +
        "  hdop=" + String(gps.hdop.hdop(), 1);
  } else s += "  (no fix)";
  return s;
}
static void handleShellLine(String line) {
  line.trim();
  // in the interactive setup wizard?
  if (wizStep >= 0) {
    if (line.length()) { String r = applyCfgCmd(String(WIZ_CMD[wizStep]) + " " + line); if (r.length()) shellPrint(r + "\r\n"); }
    wizStep++;
    if (wizStep < WIZ_N) { shellPrint(WIZ_Q[wizStep]); return; }
    wizStep = -1; shellPrint("Setup complete.\r\n"); shellPrompt(); return;
  }
  if (line.length() == 0) { shellPrompt(); return; }
  if (line == "/quit" || line == "/exit") { shellPrint("73s (bye)\r\n"); if (shellOut == (Print*)&shellClient) shellClient.stop(); return; }
  if (line == "/help" || line == "/?") {
    shellPrint("Chat: just type. Chat helpers:\r\n"
               "/cls clear screen | /history [n] | /retry | /clear wipe chat\r\n"
               "/who | /time | /gps [stream|stop] | /bat | /rssi | /status | /about\r\n"
               "/web <query> (DuckDuckGo) | /tools [name on|off] | /home <lat lon|place>\r\n"
               "/get config | /set (list options) | /wizard | /prompt <word> | /ip | /reboot | /quit\r\n"
               "Config: /name /provider /model /key <prov> <k> /wifi <ssid> <pw> /mapkey\r\n"
               "        /brightness 10-100 /tz <hrs> /sounds on|off /trackball on|off /status ip|demo|phone\r\n"
               "Apps: /map [lat lon] | /snake | /sudoku | /slide\r\n");
    shellPrompt(); return;
  }
  if (line == "/cls") { shellPrint("\033[2J\033[H"); shellBanner(); shellPrompt(); return; }   // clear terminal screen
  if (line == "/gps" || line == "/gps status") { shellPrint(gpsLine() + "\r\n"); shellPrompt(); return; }
  if (line == "/gps stream" || line == "/gps on") {
    gpsStream = true; streamOut = shellOut; gpsStreamT = 0;
    shellPrint("GPS streaming every 2s — type /gps stop to end.\r\n"); return;
  }
  if (line == "/gps stop" || line == "/gps off") { gpsStream = false; shellPrint("GPS stream stopped.\r\n"); shellPrompt(); return; }
  if (line == "/bat" || line == "/battery") {
    shellPrint("battery: " + String(batteryPct()) + "%" + (batteryCharging() ? " (charging)" : "") + "\r\n"); shellPrompt(); return;
  }
  if (line == "/rssi" || line == "/signal") {
    shellPrint("wifi: " + String(WiFi.RSSI()) + "dBm  bars=" + String(wifiLevel()) + "/4\r\n"); shellPrompt(); return;
  }
  if (line == "/reboot") { shellPrint("rebooting...\r\n"); delay(200); ESP.restart(); return; }
  if (line == "/about") {
    shellPrint("RoostOS Communicator " + String(ROOST_COMM_VERSION) + "\r\n"
               "Web:    roostos.dev/tdeck\r\nGitHub: github.com/StevenSSparks/roost-tdeck\r\n"
               "Device: T-Deck Plus (ESP32-S3)   MAC: " + WiFi.macAddress() + "\r\n"
               "Heap: " + String(ESP.getFreeHeap()/1024) + "K  PSRAM: " + String(ESP.getFreePsram()/1024) + "K  up: " + String(millis()/1000) + "s\r\n");
    shellPrompt(); return;
  }
  if (line == "/status") {
    shellPrint("provider: " + aiProvider + "/" + aiModel + "   you: " + youLabel + "\r\n"
               "wifi: " + (WiFi.isConnected() ? WiFi.SSID() : String("down")) + " " + String(WiFi.RSSI()) + "dBm (" + String(wifiLevel()) + "/4)  ip " + WiFi.localIP().toString() + "\r\n"
               "battery: " + String(batteryPct()) + "%" + (batteryCharging() ? "+" : "") + "   " + gpsLine() + "\r\n"
               "ssh: " + String(sshOn ? "on" : "off") + "  shell: " + String(remoteShellOn ? "on" : "off") + "  statusbar: " + statusName() + "\r\n");
    shellPrompt(); return;
  }
  if (line.startsWith("/status ")) { String r = applyCfgCmd(line.substring(1)); shellPrint(r + "\r\n"); shellPrompt(); return; }
  if (line == "/tools" || line.startsWith("/tools ")) {
    String a = line.length() > 6 ? line.substring(7) : String(""); a.trim();
    if (!a.length()) { shellPrint(String("AI tools:\r\n  show_map    ") + (toolShowMap ? "on" : "off") +
                                  "\r\n  get_location " + (toolGetLoc ? "on" : "off") +
                                  "\r\n(toggle: /tools <name> on|off)\r\n"); shellPrompt(); return; }
    int sp = a.indexOf(' '); String nm = sp < 0 ? a : a.substring(0, sp); String v = sp < 0 ? "" : a.substring(sp + 1); v.trim();
    bool on = (v == "on" || v == "1" || v == "");
    if (nm.startsWith("show") || nm == "map") toolShowMap = on;
    else if (nm.startsWith("get") || nm == "location" || nm == "loc" || nm == "gps") toolGetLoc = on;
    else { shellPrint("tools: show_map | get_location\r\n"); shellPrompt(); return; }
    saveCfg(); shellPrint("tool " + nm + ": " + (on ? "on" : "off") + "\r\n"); shellPrompt(); return;
  }
  if (line == "/web" || line.startsWith("/web ")) {
    String q = line.length() > 4 ? line.substring(5) : String(""); q.trim();
    if (!q.length()) { shellPrint("usage: /web <query>\r\n"); shellPrompt(); return; }
    shellPrint("searching DuckDuckGo...\r\n");
    String res = ddgSearch(q);
    String p = res.length()
      ? "Web search results for \"" + q + "\" (via DuckDuckGo):\n" + res +
        "\nAnswer the query concisely, drawing on these results plus what you know."
      : "Answer concisely from your own knowledge: \"" + q +
        "\". (A web lookup returned nothing usable — just answer directly, don't mention search results.)";
    String reply = deMarkdown(askAI(p));
    addMsg(youLabel + " (web): " + q, userColor); addMsg(aiLabel() + ": " + reply, aiColor);
    if (uiMode == MODE_CHAT) { scrollLines = 0; draw(); }
    shellPrint(reply + "\r\n(via DuckDuckGo)\r\n"); shellPrompt(); return;
  }
  if (line == "/who") { shellPrint("provider: " + aiProvider + " / " + aiModel + "   you: " + youLabel + "\r\n"); shellPrompt(); return; }
  if (line == "/time") {
    time_t t = time(nullptr);
    if (t > 1700000000) { t += (time_t)tzOffsetMin * 60; struct tm tm; gmtime_r(&t, &tm);
      char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm); shellPrint(String(b) + " (local)\r\n"); }
    else shellPrint("time not synced (no NTP yet)\r\n");
    shellPrompt(); return;
  }
  if (line == "/history" || line.startsWith("/history ") || line == "/log") {
    int n = 12; int sp = line.indexOf(' '); if (sp > 0) { int v = line.substring(sp + 1).toInt(); if (v > 0) n = v; }
    int start = (int)msgs.size() > n ? (int)msgs.size() - n : 0;
    for (int i = start; i < (int)msgs.size(); i++) shellPrint(msgs[i].text + "\r\n");
    if (msgs.empty()) shellPrint("(no chat yet)\r\n");
    shellPrompt(); return;
  }
  if (line == "/retry" || line == "/r") {
    if (!lastPrompt.length()) { shellPrint("nothing to retry\r\n"); shellPrompt(); return; }
    line = lastPrompt;   // fall through to the chat path below
  }
  else if (line == "/set" || line.startsWith("/set ")) {
    String rest = line.length() > 4 ? line.substring(5) : String(""); rest.trim();
    if (!rest.length()) {                              // list the settable options
      shellPrint("Set an option:  /set <option> <value>\r\n"
                 "  name <you> | you <label> | tz <hours> | brightness <10-100>\r\n"
                 "  provider anthropic|openai|gemini|ollama | model <name>\r\n"
                 "  key anthropic|openai|gemini <apikey> | ollama <host:port> | mapkey <key>\r\n"
                 "  wifi <ssid> <password> | status ip|demo|phone\r\n"
                 "  sounds on|off | trackball on|off | websearch on|off\r\n"
                 "  ssh on|off | sshuser <name> | sshpass <password>\r\n"
                 "  font tiny|small|medium|large | color user|ai|accent <name> | scroll <n> | splash <ms>\r\n"
                 "Or /wizard for guided step-by-step setup.\r\n");
      shellPrompt(); return;
    }
    String r = applyCfgCmd(rest);                      // apply one option
    shellPrint((r.length() ? r : String("set: unknown option (type /set to list)")) + "\r\n"); shellPrompt(); return;
  }
  else if (line == "/wizard") { wizStep = 0; shellPrint("Guided setup - blank answer skips each.\r\n" + String(WIZ_Q[0])); return; }
  else if (line == "/get")  { shellPrint(shellConfigSummary()); shellPrompt(); return; }
  else if (line == "/ip")   { shellPrint("ip=" + WiFi.localIP().toString() + "\r\n"); shellPrompt(); return; }
  else if (line.startsWith("/")) {
    String r = applyCfgCmd(line.substring(1));
    shellPrint((r.length() ? r : String("unknown command (try /help)")) + "\r\n"); shellPrompt(); return;
  }
  // free text => chat (shared with the on-screen buffer)
  lastPrompt = line;   // remember for /retry
  addMsg(youLabel + " (ssh): " + line, userColor);
  if (uiMode == MODE_CHAT) { scrollLines = 0; draw(); }
  shellPrint("...\r\n");
  String reply = deMarkdown(askAI(line));
  addMsg(aiLabel() + ": " + reply, aiColor);
  if (uiMode == MODE_CHAT) { scrollLines = 0; draw(); }
  shellPrint(aiLabel() + ": " + reply + "\r\n");
  if (pendingMap) { pendingMap = false; showMap(pMapLat, pMapLon, 13); shellPrint("(map shown on device)\r\n"); }
  shellPrompt();
}
static void pollShell() {
  if (!shellClient || !shellClient.connected()) {
    WiFiClient c = shellServer.available();
    if (c) { shellClient = c; shellBuf = ""; wizStep = -1; shellOut = (Print*)&shellClient; shellBanner(); shellPrompt(); }
    return;
  }
  shellOut = (Print*)&shellClient;
  while (shellClient.available()) {
    char c = shellClient.read();
    if (c == '\r' || c == '\n') {
      if (c == '\n' && shellLastCR) { shellLastCR = false; continue; }  // swallow \n after \r
      shellLastCR = (c == '\r');
      String ln = shellBuf; shellBuf = ""; handleShellLine(ln);
    } else { shellLastCR = false;
      if (c == 8 || c == 127) { if (shellBuf.length()) shellBuf.remove(shellBuf.length() - 1); }
      else if ((uint8_t)c >= 32 && (uint8_t)c < 127) shellBuf += c;
    }
  }
}

// ============================================================================
//  Games: Snake (trackball/WASD) + Sudoku (QWERTY). MODE_GAME; Esc/q = back.
// ============================================================================
static int gGame = 0;               // 0 = snake, 1 = sudoku, 2 = slide
// --- Snake ---
static const int CELL = 12;
static const int SNK_CTRL = 46;     // bottom control-bar (D-pad) height
static int gCols, gRows;
static std::vector<std::pair<int,int>> snake;
static int sdx = 1, sdy = 0, ndx = 1, ndy = 0, gScore = 0;
static bool gDead = false, gStarted = false;   // waits for first input before moving
static uint32_t gTickT = 0;
static void placeFood(int& fx, int& fy) {
  bool ok; do { ok = true; fx = random(gCols); fy = random(gRows);
    for (auto& s : snake) if (s.first == fx && s.second == fy) ok = false;
  } while (!ok);
}
static int gFoodX, gFoodY;
static void snakeInit() {
  gCols = scrW / CELL; gRows = (scrH - headerH - SNK_CTRL) / CELL;
  snake.clear(); snake.push_back({gCols / 2, gRows / 2});
  sdx = ndx = 1; sdy = ndy = 0; gScore = 0; gDead = false; gStarted = false;
  placeFood(gFoodX, gFoodY); gTickT = millis();
}
// Shared game header buttons: [new] and [X], top-right. Hit zones (in touch):
//   new: scrW-54..scrW-18 ;  X: scrW-18..scrW , both sy < headerH+2.
static void drawGameBtns() {
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(MC_DATUM);
  tft.fillRect(scrW - 54, 1, 36, headerH - 1, C_BG);
  tft.drawRect(scrW - 54, 1, 35, headerH - 2, C_DIM);
  tft.setTextColor(C_TEAL, C_BG); tft.drawString("new", scrW - 36, headerH / 2);
  tft.fillRect(scrW - 16, 1, 15, headerH - 1, C_BG);
  tft.drawRect(scrW - 16, 1, 14, headerH - 2, C_DIM);
  tft.setTextColor(C_AMBER, C_BG); tft.drawString("X", scrW - 9, headerH / 2);
  tft.setTextDatum(TL_DATUM);
}
// Snake D-pad geometry: 4 buttons  <  ^  v  >  across the bottom control bar.
static const int SNK_DIRS[4][2] = {{-1,0},{0,-1},{0,1},{1,0}};
static int snakeDpadHit(int sx, int sy) {   // returns 0..3 or -1
  if (sy < scrH - SNK_CTRL) return -1;
  int i = sx / (scrW / 4); return (i < 0) ? 0 : (i > 3 ? 3 : i);
}
static void snakeDraw() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("Snake", 4, 4);
  tft.setTextColor(C_INK, C_PANEL); tft.drawString("score " + String(gScore), 52, 4);
  drawGameBtns();
  int oy = headerH;
  tft.fillRect(gFoodX * CELL, oy + gFoodY * CELL, CELL - 1, CELL - 1, C_AMBER);
  for (size_t i = 0; i < snake.size(); i++)
    tft.fillRect(snake[i].first * CELL, oy + snake[i].second * CELL, CELL - 1, CELL - 1, i == 0 ? C_TEAL : C_INDIGO);
  // D-pad control bar
  int cy = scrH - SNK_CTRL, bw = scrW / 4;
  const char* lbl[4] = {"<", "^", "v", ">"};
  tft.fillRect(0, cy, scrW, SNK_CTRL, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < 4; i++) {
    int bx = i * bw;
    tft.drawRoundRect(bx + 2, cy + 3, bw - 4, SNK_CTRL - 6, 5, C_TEAL);
    tft.setFreeFont(&FreeSans12pt7b); tft.setTextColor(C_INK, C_PANEL);
    tft.drawString(lbl[i], bx + bw / 2, cy + SNK_CTRL / 2);
  }
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  int midY = (headerH + (scrH - SNK_CTRL)) / 2;
  if (!gStarted && !gDead) {                     // waiting to start
    tft.setTextDatum(MC_DATUM); tft.setTextColor(C_AMBER, C_BG);
    tft.drawString("press an arrow to start", scrW / 2, midY);
    tft.setTextDatum(TL_DATUM);
  }
  if (gDead) {                                   // GAME OVER + restart prompt (in play area)
    int bw2 = 200, bh = 66, bx = (scrW - bw2) / 2, by = midY - bh / 2;
    tft.fillRoundRect(bx, by, bw2, bh, 6, C_PANEL);
    tft.drawRoundRect(bx, by, bw2, bh, 6, C_TEAL);
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&FreeSans12pt7b); tft.setTextColor(C_AMBER, C_PANEL);
    tft.drawString("GAME OVER", scrW / 2, by + 18);
    tft.setTextFont(1); tft.setTextSize(1);
    tft.setTextColor(C_INK, C_PANEL); tft.drawString("score " + String(gScore), scrW / 2, by + 38);
    tft.setTextColor(C_DIM, C_PANEL); tft.drawString("tap 'new' / arrow / key to replay", scrW / 2, by + 52);
    tft.setTextDatum(TL_DATUM);
  }
}
static void snakeStep() {
  if (gDead) return;
  sdx = ndx; sdy = ndy;
  int hx = snake[0].first + sdx, hy = snake[0].second + sdy;
  if (hx < 0 || hy < 0 || hx >= gCols || hy >= gRows) { gDead = true; snakeDraw(); return; }
  for (auto& s : snake) if (s.first == hx && s.second == hy) { gDead = true; snakeDraw(); return; }
  snake.insert(snake.begin(), {hx, hy});
  if (hx == gFoodX && hy == gFoodY) { gScore++; placeFood(gFoodX, gFoodY); }
  else snake.pop_back();
  snakeDraw();
}
static void snakeTurn(int dx, int dy) {   // ignore 180-degree reversals
  gStarted = true;                        // first input starts the snake moving
  if (dx == -sdx && dy == -sdy) return;
  ndx = dx; ndy = dy;
}
// --- Sudoku ---
static const char* SU_PUZZLES[] = {
  "530070000600195000098000060800060003400803001700020006060000280000419005000080079",
  "004300209005009001070060043006002087190007400050083000600000105003508690042910300",
  "000000907000420180000705026100904000050000040000507009920108000034059000507000000",
  "030050040008010500460000012070502080000603000040109030250000098001020600080060020",
  "020810740700003100090002805009040087400208003160030200302700060005600008076051090",
  "100920000524010000000000070050008102000000000402700090060000000000030945000071006",
  "043080250600000000000001094900004070000608000010200003820500000000000005034090710",
  "480006902002008001900370060840010200003704100001060049020085007700900600609200018",
  "000900002050123400030000160908000000070000090000000205091000050007439020400007000",
  "001900003900700160030005007050000009004302600200000070600100030042007006500006800",
};
static const int SU_NPUZZLE = 10;
static int su[81], suGiven[81], suCur = 0;
static void sudokuInit() {
  const char* pz = SU_PUZZLES[random(SU_NPUZZLE)];   // pick one of ~10 boards
  for (int i = 0; i < 81; i++) { su[i] = pz[i] - '0'; suGiven[i] = su[i] != 0; }
  suCur = 0;
}
static bool sudokuWon() {
  for (int i = 0; i < 81; i++) if (su[i] == 0) return false;
  for (int r = 0; r < 9; r++) for (int a = 0; a < 9; a++) for (int b = a + 1; b < 9; b++) {
    if (su[r*9+a] == su[r*9+b]) return false;      // row
    if (su[a*9+r] == su[b*9+r]) return false;      // col
  }
  for (int br = 0; br < 9; br += 3) for (int bc = 0; bc < 9; bc += 3) {
    int seen[10] = {0};
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { int v = su[(br+r)*9 + bc+c]; if (seen[v]++) return false; }
  }
  return true;
}
// Sudoku layout (shared by draw + touch): 20px grid + a number-pad row at bottom.
static const int SU_G = 20, SU_OY = 18;
static int suOX() { return (scrW - SU_G * 9) / 2; }
static int suPadY() { return SU_OY + SU_G * 9 + 4; }   // top of number pad
static int suPadBW() { return scrW / 10; }
static void sudokuDraw() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("Sudoku", 4, 4);
  bool won = sudokuWon();
  tft.setTextColor(won ? C_TEAL : C_DIM, C_PANEL);
  tft.drawString(won ? "SOLVED!" : "tap cell, tap number", 84, 4);
  drawGameBtns();
  int G = SU_G, ox = suOX(), oy = SU_OY;
  for (int i = 0; i < 81; i++) {
    int r = i / 9, c = i % 9, x = ox + c * G, y = oy + r * G;
    if (i == suCur) tft.fillRect(x, y, G, G, C_PANEL);
    tft.drawRect(x, y, G, G, C_DIM);
    if (su[i]) { tft.setFreeFont(&FreeSans9pt7b); tft.setTextDatum(MC_DATUM);
      tft.setTextColor(suGiven[i] ? C_INK : C_AMBER, i == suCur ? C_PANEL : C_BG);
      tft.drawString(String(su[i]), x + G / 2, y + G / 2 + 1);
      tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
    }
  }
  for (int k = 0; k <= 9; k += 3) {                 // thick 3x3 separators
    tft.drawFastVLine(ox + k * G, oy, G * 9, C_TEAL);
    tft.drawFastHLine(ox, oy + k * G, G * 9, C_TEAL);
  }
  // number pad: 1-9 then C(lear)
  int ny = suPadY(), bw = suPadBW(), bh = scrH - ny - 1;
  tft.setTextDatum(MC_DATUM);
  for (int d = 0; d < 10; d++) {
    int bx = d * bw;
    tft.drawRoundRect(bx + 1, ny, bw - 2, bh, 3, C_INDIGO);
    tft.setTextColor(d < 9 ? C_INK : C_DIM, C_BG);
    tft.drawString(d < 9 ? String(d + 1) : "C", bx + bw / 2, ny + bh / 2);
  }
  tft.setTextDatum(TL_DATUM);
}
// --- Sliding puzzle: tiles 1..11 on a 3x4 grid, one blank; tap a tile next to
//     the gap to slide it. Shuffled via random legal moves (always solvable). ---
static int slide[12];
static int slideBlank() { for (int i = 0; i < 12; i++) if (slide[i] == 0) return i; return 11; }
static void slideInit() {
  for (int i = 0; i < 12; i++) slide[i] = (i < 11) ? i + 1 : 0;
  int b = 11;
  for (int n = 0; n < 300; n++) {
    int nb[4], c = 0, r = b / 3, col = b % 3;
    if (r > 0) nb[c++] = b - 3; if (r < 3) nb[c++] = b + 3;
    if (col > 0) nb[c++] = b - 1; if (col < 2) nb[c++] = b + 1;
    int t = nb[random(c)]; slide[b] = slide[t]; slide[t] = 0; b = t;
  }
}
static bool slideWon() { for (int i = 0; i < 11; i++) if (slide[i] != i + 1) return false; return slide[11] == 0; }
static void slideGeom(int& cw, int& ch, int& ox, int& oy) { cw = 90; ch = 52; ox = (scrW - cw * 3) / 2; oy = headerH + 4; }
static void slideDraw() {
  tft.fillScreen(C_BG); tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("Slide 1-11", 4, 4);
  bool won = slideWon();
  tft.setTextColor(won ? C_TEAL : C_DIM, C_PANEL); tft.drawString(won ? "SOLVED!" : "tap a tile by the gap", 96, 4);
  drawGameBtns();
  int cw, ch, ox, oy; slideGeom(cw, ch, ox, oy);
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < 12; i++) {
    if (slide[i] == 0) continue;
    int x = ox + (i % 3) * cw, y = oy + (i / 3) * ch;
    tft.fillRoundRect(x + 2, y + 2, cw - 4, ch - 4, 5, won ? C_TEAL : C_INDIGO);
    tft.setFreeFont(&FreeSans12pt7b); tft.setTextColor(C_BG, won ? C_TEAL : C_INDIGO);
    tft.drawString(String(slide[i]), x + cw / 2, y + ch / 2);
    tft.setTextFont(1); tft.setTextSize(1);
  }
  tft.setTextDatum(TL_DATUM);
}
static void slideTap(int sx, int sy) {
  int cw, ch, ox, oy; slideGeom(cw, ch, ox, oy);
  if (sx < ox || sy < oy) return;
  int c = (sx - ox) / cw, r = (sy - oy) / ch;
  if (c < 0 || c > 2 || r < 0 || r > 3) return;
  int idx = r * 3 + c, b = slideBlank(), br = b / 3, bc = b % 3;
  if ((r == br && abs(c - bc) == 1) || (c == bc && abs(r - br) == 1)) { slide[b] = slide[idx]; slide[idx] = 0; slideDraw(); }
}
static void gameLaunch(int which) {
  gGame = which; uiMode = MODE_GAME;
  if (which == 0)      { snakeInit();  snakeDraw(); }
  else if (which == 1) { sudokuInit(); sudokuDraw(); }
  else                 { slideInit();  slideDraw(); }
}
static void gameKey(uint8_t k) {
  if (k == 'q' || k == 27) { uiMode = MODE_CHAT; draw(); return; }
  if (gGame == 0) {   // snake
    if (gDead) { snakeInit(); snakeDraw(); return; }
    if (k == 'w') snakeTurn(0, -1); else if (k == 's') snakeTurn(0, 1);
    else if (k == 'a') snakeTurn(-1, 0); else if (k == 'd') snakeTurn(1, 0);
  } else {            // sudoku
    if (k == 'w' && suCur >= 9) suCur -= 9;
    else if (k == 's' && suCur < 72) suCur += 9;
    else if (k == 'a' && suCur % 9) suCur--;
    else if (k == 'd' && suCur % 9 != 8) suCur++;
    else if (k >= '1' && k <= '9') { if (!suGiven[suCur]) su[suCur] = k - '0'; }
    else if (k == '0' || k == ' ' || k == 8 || k == 127) { if (!suGiven[suCur]) su[suCur] = 0; }
    sudokuDraw();
  }
}
static void gameTick() {   // snake auto-advance; starts gentle, speeds up with score
  int period = 320 - gScore * 8; if (period < 130) period = 130;   // gentle start, ramps up
  if (gGame == 0 && gStarted && !gDead && millis() - gTickT > (uint32_t)period) { gTickT = millis(); snakeStep(); }
}

void loop() {
  static bool clkPrev = true; static uint32_t clkT = 0, lastTouch = 0;
  uint32_t now = millis();

  // remote shell lifecycle (start/stop with the Settings toggle)
  if (remoteShellOn && WiFi.status() == WL_CONNECTED && !shellStarted) { shellServer.begin(); shellServer.setNoDelay(true); shellStarted = true; Serial.println("[shell] listening on :23"); }
  else if ((!remoteShellOn || WiFi.status() != WL_CONNECTED) && shellStarted) { if (shellClient) shellClient.stop(); shellServer.end(); shellStarted = false; }
  if (shellStarted) pollShell();

  // real SSH server lifecycle + input marshaling (runs in its own task)
  if (sshOn && sshUser.length() && sshPass.length() && WiFi.status() == WL_CONNECTED) {
    static String lastU, lastP;                      // only push creds when they change
    if (sshUser != lastU || sshPass != lastP) { sshSetCreds(sshUser.c_str(), sshPass.c_str()); lastU = sshUser; lastP = sshPass; }
    if (!sshIsRunning()) sshServerStart();
  } else if (!sshOn && sshIsRunning()) sshServerStop();
  { static bool sshWas = false; bool sa = sshActive();
    if (sa && !sshWas) { shellOut = (Print*)&sshSink; shellBanner(); shellPrompt(); }
    sshWas = sa;
    String sl; while (sshPopLine(sl)) { shellOut = (Print*)&sshSink; handleShellLine(sl); } }

  pollGps();
  if (gpsStream && streamOut && now - gpsStreamT > 2000) { gpsStreamT = now; streamOut->print(gpsLine()); streamOut->print("\r\n"); }
  if (uiMode == MODE_GAME) gameTick();

  // charge-connected detection (no dedicated pin; infer from a voltage rise)
  { static bool prevChg = false; static uint32_t chgT = 0;
    if (now - chgT > 1500) { chgT = now;
      bool c = batteryCharging();
      if (c && !prevChg && uiMode == MODE_CHAT) { drawChargeSplash(); delay(1400); draw(); }
      prevChg = c;
    } }

  // trackball CLICK (GPIO0): chat -> open settings; settings -> activate selection
  bool clk = digitalRead(TB_CLICK);
  if (!clk && clkPrev && now - clkT > 220) {
    clkT = now; Serial.println("[click]");
    if (uiMode == MODE_CHAT) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }
    else if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings(); }
    else if (uiMode == MODE_MAP) { uiMode = MODE_CHAT; draw(); }
    else if (uiMode == MODE_TEXT) { uiMode = textReturnMode;   // click = cancel text entry
      if (uiMode == MODE_SETTINGS) drawSettings(); else draw(); }
    else if (uiMode != MODE_GAME) activateSetting();   // (games handle click via their own touch)
  }
  clkPrev = clk;

  // trackball ROLL up/down (scroll in chat; move selection in settings) — gated by Settings toggle
  bool u = digitalRead(TB_UP), d = digitalRead(TB_DOWN);
  if (trackballOn) {
    if (uiMode == MODE_CHAT) {
      int before = scrollLines;
      if (!u && tbUpPrev && now - lastScroll > 90) { scrollLines += scrollStep; lastScroll = now; }
      if (!d && tbDnPrev && now - lastScroll > 90) { scrollLines -= scrollStep; lastScroll = now; }
      if (scrollLines < 0) scrollLines = 0;
      if (scrollLines != before) draw();
    } else if (uiMode == MODE_SETTINGS) {
      if (!u && tbUpPrev && now - lastScroll > 150) { selIdx = (selIdx - 1 + pageLen(setPage)) % pageLen(setPage); lastScroll = now; drawSettings(); }
      if (!d && tbDnPrev && now - lastScroll > 150) { selIdx = (selIdx + 1) % pageLen(setPage); lastScroll = now; drawSettings(); }
    } else if (uiMode == MODE_GAME && gGame == 0) {   // snake steering (edge-triggered)
      static bool lPrev = true, rPrev = true;
      bool lft = digitalRead(1), rgt = digitalRead(2);   // TB_LEFT=GPIO1, TB_RIGHT=GPIO2
      if (!u && tbUpPrev) snakeTurn(0, -1);
      if (!d && tbDnPrev) snakeTurn(0, 1);
      if (!lft && lPrev) snakeTurn(-1, 0);
      if (!rgt && rPrev) snakeTurn(1, 0);
      lPrev = lft; rPrev = rgt;
    }
  }
  tbUpPrev = u; tbDnPrev = d;

  // CALIBRATION capture: sample every loop, average a press, commit on release
  // The GT911 "data ready" flag clears on each read, so a single poll flickers
  // true/false while a finger is held. Debounce with hysteresis: latch the first
  // good read as a press, act once, and only treat it as released after ~160ms
  // with no touch. This fixes both missed quick taps and menu flip-flop.
  {
    static bool tPressed = false, tActed = false;
    static uint32_t tSeen = 0, tPoll = 0; static int tRX = 0, tRY = 0;
    int rx = 0, ry = 0; bool touched = false;
    if (now - tPoll >= 35) { tPoll = now; touched = gtReadRaw(rx, ry); }  // throttle I2C (ESP32 headroom for WiFi/SSH)
    if (touched) { tSeen = now; tRX = rx; tRY = ry; if (!tPressed) { tPressed = true; tActed = false; } }
    else if (tPressed && now - tSeen > 160) tPressed = false;   // debounced release
    bool justPressed = tPressed && !tActed;

    if (uiMode == MODE_CALIB) {
      if (justPressed) { tActed = true; Serial.printf("[cal] TAP step %d raw=%d,%d\n", calStep, tRX, tRY); calRecord(tRX, tRY); }
    } else if (justPressed) {
      tActed = true;
      int sx, sy; gtMap(tRX, tRY, sx, sy);
      Serial.printf("[touch] raw=%d,%d screen=%d,%d mode=%d\n", tRX, tRY, sx, sy, uiMode);
      if (uiMode == MODE_CHAT) {
        if (sy < 40) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }  // tap top ~40px = menu
        else if (sx > scrW - 36 && sy > scrH - 26) clearChat();   // bottom-right = clear chat
      } else if (uiMode == MODE_MAP) {
        uiMode = MODE_CHAT; draw();                 // tap anywhere closes the map
      } else if (uiMode == MODE_GAME) {
        if (sy < headerH + 2 && sx > scrW - 18) { uiMode = MODE_CHAT; draw(); }   // [X] quit
        else if (sy < headerH + 2 && sx > scrW - 54) {                            // [new] restart this game
          if (gGame == 0) { snakeInit(); snakeDraw(); }
          else if (gGame == 1) { sudokuInit(); sudokuDraw(); }
          else { slideInit(); slideDraw(); }
        }
        else if (gGame == 0) {                      // Snake: D-pad / tap-to-steer / restart
          if (gDead) { snakeInit(); snakeDraw(); }
          else {
            int dp = snakeDpadHit(sx, sy);
            if (dp >= 0) snakeTurn(SNK_DIRS[dp][0], SNK_DIRS[dp][1]);   // D-pad button
            else {                                                      // tap in field: steer toward it
              int oy = headerH;
              int hcx = snake[0].first * CELL + CELL / 2, hcy = oy + snake[0].second * CELL + CELL / 2;
              int ddx = sx - hcx, ddy = sy - hcy;
              if (abs(ddx) > abs(ddy)) snakeTurn(ddx > 0 ? 1 : -1, 0);
              else                     snakeTurn(0, ddy > 0 ? 1 : -1);
            }
          }
        }
        else if (gGame == 1) {                      // Sudoku: tap cell, then tap a number
          int G = SU_G, ox = suOX(), oy = SU_OY, ny = suPadY(), bw = suPadBW();
          if (sy >= oy && sy < oy + G * 9 && sx >= ox && sx < ox + G * 9) {
            suCur = ((sy - oy) / G) * 9 + (sx - ox) / G; sudokuDraw();
          } else if (sy >= ny) {
            int d = sx / bw;
            if (!suGiven[suCur]) su[suCur] = (d < 9) ? d + 1 : 0;   // 1-9 or C=clear
            sudokuDraw();
          }
        } else if (gGame == 2) slideTap(sx, sy);   // sliding puzzle
      } else if (uiMode == MODE_TEXT) {
        if (sy < headerH + 4 && sx > scrW - 18) {   // tap [X] = cancel entry
          uiMode = textReturnMode; if (uiMode == MODE_SETTINGS) drawSettings(); else draw();
        }
      } else if (uiMode == MODE_ABOUT) {
        uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings();
      } else if (uiMode == MODE_SETTINGS) {
        setFont(1); int lh = tft.fontHeight() + 6; int row = setFirst + (sy - (headerH + 4)) / lh;
        if (row >= 0 && row < pageLen(setPage)) { selIdx = row; activateSetting(); }
      }
    }
  }

  // KEYBOARD
  uint8_t k = readKey();
  if (k) {
    if (uiMode == MODE_TEXT) {
      if (k == '\r' || k == '\n') {
        auto cb = textCb; String v = textVal;
        uiMode = textReturnMode;              // default landing (cb may re-open text)
        if (cb) cb(v);
        if (uiMode == MODE_TEXT) drawText();
        else if (uiMode == MODE_SETTINGS) drawSettings();
        else draw();
      } else if (k == 27) {                   // Esc = cancel
        uiMode = textReturnMode;
        if (uiMode == MODE_SETTINGS) drawSettings(); else draw();
      } else if (k == 8 || k == 127) { if (textVal.length()) textVal.remove(textVal.length() - 1); drawText(); }
      else if (k >= 32 && k < 127) { textVal += (char)k; drawText(); }
    }
    else if (uiMode == MODE_GAME) { gameKey(k); }
    else if (uiMode == MODE_CALIB) { uiMode = MODE_CHAT; draw(); }      // any key cancels calibration
    else if (uiMode == MODE_MAP) { uiMode = MODE_CHAT; draw(); }        // any key exits map
    else if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings(); }
    else if (uiMode == MODE_SETTINGS) { uiMode = MODE_CHAT; draw(); }   // any key exits settings
    else if (k == '\r' || k == '\n') {
      if (input.length()) {
        String p = input; input = "";
        if (p.startsWith("/")) {
          String r = applyCfgCmd(p.substring(1));
          if (uiMode == MODE_CHAT) { addMsg(r.length() ? r : ("unknown: " + p), C_DIM); scrollLines = 0; draw(); }
        } else sendPrompt(p);
      }
    }
    else if (k == 8 || k == 127) { if (input.length()) { input.remove(input.length() - 1); draw(); } }
    else if (k >= 32 && k < 127) { input += (char)k; draw(); }
  }

  // USB-C SERIAL = the same interactive shell ("SSH over USB"): chat + /set + /get
  // + all /commands, right in a serial monitor. Test verbs (click/up/down) kept.
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      String s = serialBuf; serialBuf = "";
      if (s == "ip")
        Serial.printf("ip=%s status=%d mode=%d rssi=%ddBm lvl=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.status(), uiMode, (int)WiFi.RSSI(), wifiLevel());
      else if (s == "click") { if (uiMode == MODE_CHAT) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); } else activateSetting(); Serial.printf("mode=%d sel=%d\n", uiMode, selIdx); }
      else if (s == "down")  { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx + 1) % pageLen(setPage); drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else if (s == "up")    { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx - 1 + pageLen(setPage)) % pageLen(setPage); drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else {
        shellOut = (Print*)&Serial;
        if (s.startsWith("ask ")) handleShellLine(s.substring(4));
        else if (s.startsWith("claude ")) handleShellLine(s.substring(7));
        else if (s.startsWith("/") || wizStep >= 0) handleShellLine(s);
        else { String r = applyCfgCmd(s);          // bare config verb?
               if (r.length()) Serial.println(r);
               else handleShellLine(s); }           // otherwise: chat
      }
    } else serialBuf += c;
  }
  delay(15);   // ease loop rate; leaves the ESP32 more time for WiFi/SSH tasks
}
