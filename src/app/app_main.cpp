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
#include <Preferences.h>
#include <vector>
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

static const char* SYS_PROMPT =
  "You are RoostOS, a friendly assistant on a small handheld device with a tiny "
  "pixel-font screen. Reply in plain ASCII text only: no markdown, no headings, "
  "no bullet symbols, no emoji, no special/unicode characters. Keep replies short.";

static const char* PROVIDERS[] = {"anthropic", "openai", "gemini", "ollama"};
static const int NPROV = 4;
static String defaultModel(const String& p) {
  if (p == "openai") return "gpt-4o-mini";
  if (p == "gemini") return "gemini-2.0-flash-lite";   // small/cheap (demo key)
  if (p == "ollama") return "llama3.2";
  return "claude-haiku-4-5";
}
// A provider is selectable only if its key/host is configured (in secrets.h).
static bool providerConfigured(const String& p) {
  if (p == "anthropic") return strlen(ANTHROPIC_API_KEY) > 0;
  if (p == "openai")    return strlen(OPENAI_API_KEY) > 0;
  if (p == "gemini")    return strlen(GEMINI_API_KEY) > 0;
  if (p == "ollama")    return strlen(OLLAMA_HOST) > 0;
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
static uint16_t userColor, aiColor;   // set in setup(); configurable
static int splashMs   = 3500;  // boot splash duration
static int scrollStep = 2;     // lines per trackball detent (adjustable "rate")
static String aiProvider = DEFAULT_AI_PROVIDER;   // anthropic|openai|gemini|ollama
static String aiModel = "";                        // set from NVS / provider default

static void saveCfg() {
  prefs.begin("roostcomm", false);
  prefs.putInt("chatFont", chatFontIdx);
  prefs.putInt("inputFont", inputFontIdx);
  prefs.putUShort("userCol", userColor);
  prefs.putUShort("aiCol", aiColor);
  prefs.putInt("splashMs", splashMs);
  prefs.putInt("scrollStep", scrollStep);
  prefs.putString("aiProv", aiProvider);
  prefs.putString("aiModel", aiModel);
  prefs.end();
}
static void loadCfg() {   // call AFTER colors + defaults are set
  prefs.begin("roostcomm", true);
  chatFontIdx  = prefs.getInt("chatFont", chatFontIdx);
  inputFontIdx = prefs.getInt("inputFont", inputFontIdx);
  userColor    = prefs.getUShort("userCol", userColor);
  aiColor      = prefs.getUShort("aiCol", aiColor);
  splashMs     = prefs.getInt("splashMs", splashMs);
  scrollStep   = prefs.getInt("scrollStep", scrollStep);
  aiProvider   = prefs.getString("aiProv", aiProvider);
  aiModel      = prefs.getString("aiModel", defaultModel(aiProvider));
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
  return 0xFFFF;  // 0xFFFF = "unknown"
}

// ---- settings screen ----
enum { MODE_CHAT, MODE_SETTINGS, MODE_ABOUT };
static int uiMode = MODE_CHAT;
static int selIdx = 0;
// Settings are organized as a main page with per-category sub-pages (BlackBerry
// style: <=6 options each, item 0 is always Back). selIdx indexes the current page.
enum { PG_MAIN, PG_DISPLAY, PG_COLORS, PG_AI, PG_SYSTEM, PG_COUNT };
static int setPage = PG_MAIN;
static String setMsg = "";   // transient status line (e.g. provider switch result)
static const char* PAL_NAMES[] = {"teal","indigo","amber","red","green","cyan","magenta","orange","ink"};
static const int NPAL = 9;
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
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(" Communicator", rx, 4);
  // menu button (hamburger) top-right — tap to open Settings
  for (int b = 0; b < 3; b++) tft.fillRect(scrW - 20, 3 + b * 4, 15, 2, C_AMBER);
  String net = WiFi.status() == WL_CONNECTED
    ? (WiFi.SSID() + " " + WiFi.localIP().toString()) : String("WiFi down");
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(net, scrW - tft.textWidth(net) - 24, 4);

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

  // input line (own font, amber prompt)
  tft.fillRect(0, inputY, scrW, inputH, C_PANEL);
  setFont(inputFontIdx);
  tft.setTextColor(C_AMBER, C_PANEL); tft.drawString("> ", 2, inputY + 2);
  int pw = tft.textWidth("> ") + 2;
  tft.setTextColor(C_INK, C_PANEL);
  String shown = input;
  while (shown.length() && tft.textWidth(shown) > maxW - pw) shown = shown.substring(1);
  tft.drawString(shown, 2 + pw, inputY + 2);
}

// ---- Settings screen: main page + per-category sub-pages ----
// forward decls (defined later in the file)
static bool switchProvider(const String& p, String& msg);
static bool providerConfigured(const String& p);
static String defaultModel(const String& p);

// Fill labels[]/values[] for a page and return its title. Item 0 is always Back.
static String buildPage(int pg, std::vector<String>& labels, std::vector<String>& values) {
  labels.clear(); values.clear();
  auto row = [&](const char* l, const String& v){ labels.push_back(l); values.push_back(v); };
  switch (pg) {
    case PG_MAIN:
      row("Back to chat", "");
      row("Display", ">"); row("Colors", ">");
      row("AI Provider", aiProvider);
      row("System / About", ">");
      return "Settings";
    case PG_DISPLAY:
      row("< Back", "");
      row("Chat font",  FONTS[chatFontIdx].name);
      row("Input font", FONTS[inputFontIdx].name);
      row("Scroll rate", String(scrollStep));
      row("Splash", String(splashMs / 1000.0, 1) + "s");
      return "Display";
    case PG_COLORS:
      row("< Back", "");
      row("Your color", PAL_NAMES[palIndexOf(userColor)]);
      row("AI color",   PAL_NAMES[palIndexOf(aiColor)]);
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
      row("WiFi", WiFi.isConnected() ? WiFi.SSID() : String("down"));
      row("IP", WiFi.localIP().toString());
      row("Uptime", String(millis() / 1000) + "s");
      return "System";
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
  int lh = tft.fontHeight() + 6, y = headerH + 6;
  for (int i = 0; i < (int)labels.size(); i++) {
    bool sel = (i == selIdx);
    if (sel) tft.fillRect(0, y - 2, scrW, lh, C_PANEL);
    uint16_t c = sel ? C_AMBER : C_INK;
    // colour swatch preview on the Colors page
    if (setPage == PG_COLORS && i == 1 && !sel) c = userColor;
    if (setPage == PG_COLORS && i == 2 && !sel) c = aiColor;
    tft.setTextColor(c, sel ? C_PANEL : C_BG);
    tft.drawString(labels[i], 6, y);
    if (values[i].length()) {
      tft.setTextColor(sel ? C_INK : C_DIM, sel ? C_PANEL : C_BG);
      tft.drawString(values[i], scrW - tft.textWidth(values[i]) - 8, y);
    }
    y += lh;
  }
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_DIM, C_BG);
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
  setFont(1);
  int y = headerH + 6, lh = tft.fontHeight() + 4;
  auto row = [&](const String& a, const String& b) {
    tft.setTextColor(C_DIM, C_BG); tft.drawString(a, 6, y);
    tft.setTextColor(C_INK, C_BG); tft.drawString(b, 96, y); y += lh;
  };
  row("Version", ROOST_COMM_VERSION);
  row("Device", "T-Deck Plus (S3)");
  row("MAC", WiFi.macAddress());
  row("IP", WiFi.localIP().toString());
  row("WiFi", WiFi.SSID() + " " + String(WiFi.RSSI()) + "dBm");
  row("Heap", String(ESP.getFreeHeap() / 1024) + "K free");
  row("PSRAM", String(ESP.getFreePsram() / 1024) + "K free");
  row("Uptime", String(millis() / 1000) + "s");
  row("Touch", gtAddr ? "GT911 0x" + String(gtAddr, HEX) : "none");
  tft.setTextColor(C_AMBER, C_BG); tft.drawString("tap / any key = back", 4, scrH - 12);
}

// pick the next model in a small per-provider preset ring
static String nextModel(const String& p, const String& cur) {
  const char* an[] = {"claude-haiku-4-5", "claude-sonnet-4-6"};
  const char* oa[] = {"gpt-4o-mini", "gpt-4o"};
  const char* ge[] = {"gemini-2.0-flash-lite", "gemini-2.0-flash"};
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

static void activateSetting() {
  setMsg = "";
  if (setPage == PG_MAIN) {
    switch (selIdx) {
      case 0: uiMode = MODE_CHAT; draw(); return;         // Back to chat
      case 1: setPage = PG_DISPLAY; selIdx = 0; break;
      case 2: setPage = PG_COLORS;  selIdx = 0; break;
      case 3: setPage = PG_AI;      selIdx = 0; break;
      case 4: setPage = PG_SYSTEM;  selIdx = 0; break;
    }
    drawSettings(); return;
  }
  // sub-pages: item 0 is Back to the main page
  if (selIdx == 0) { setPage = PG_MAIN; selIdx = 0; drawSettings(); return; }

  switch (setPage) {
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
      if (selIdx == 1) userColor = namedColor(PAL_NAMES[(palIndexOf(userColor) + 1) % NPAL]);
      if (selIdx == 2) aiColor   = namedColor(PAL_NAMES[(palIndexOf(aiColor)   + 1) % NPAL]);
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
    case PG_SYSTEM:
      if (selIdx == 1) { uiMode = MODE_ABOUT; drawAbout(); return; }   // About
      // WiFi/IP/Uptime rows are read-only status
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

// Ask the currently-selected AI provider. Returns reply text (or an "[error] ..." string).
static String askAI(const String& prompt) {
  if (WiFi.status() != WL_CONNECTED) return "[error] WiFi not connected";
  String err, p;
  JsonDocument r;

  if (aiProvider == "anthropic") {
    if (!strlen(ANTHROPIC_API_KEY)) return "[no Anthropic key — set in secrets.h]";
    JsonDocument req; req["model"] = aiModel; req["max_tokens"] = 400; req["system"] = SYS_PROMPT;
    JsonObject m = req["messages"].to<JsonArray>().add<JsonObject>(); m["role"] = "user"; m["content"] = prompt;
    String body; serializeJson(req, body);
    p = httpPostJSON(true, "https://api.anthropic.com/v1/messages", body,
                     "x-api-key", ANTHROPIC_API_KEY, "anthropic-version", "2023-06-01", err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    if (r["type"] == "error") return String("[api] ") + (const char*)(r["error"]["message"] | "");
    String t; for (JsonObject b : r["content"].as<JsonArray>()) if (b["type"] == "text") t += (const char*)(b["text"] | "");
    return t.length() ? t : "[no text]";
  }
  if (aiProvider == "openai") {
    if (!strlen(OPENAI_API_KEY)) return "[no OpenAI key — set in secrets.h]";
    JsonDocument req; req["model"] = aiModel; req["max_tokens"] = 400;
    JsonArray a = req["messages"].to<JsonArray>();
    JsonObject s = a.add<JsonObject>(); s["role"] = "system"; s["content"] = SYS_PROMPT;
    JsonObject u = a.add<JsonObject>(); u["role"] = "user"; u["content"] = prompt;
    String body; serializeJson(req, body);
    String auth = String("Bearer ") + OPENAI_API_KEY;
    p = httpPostJSON(true, "https://api.openai.com/v1/chat/completions", body,
                     "authorization", auth.c_str(), nullptr, nullptr, err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    if (r["error"]) return String("[api] ") + (const char*)(r["error"]["message"] | "");
    return String((const char*)(r["choices"][0]["message"]["content"] | "[no text]"));
  }
  if (aiProvider == "gemini") {
    if (!strlen(GEMINI_API_KEY)) return "[no Gemini key — set in secrets.h]";
    JsonDocument req;
    req["systemInstruction"]["parts"][0]["text"] = SYS_PROMPT;
    JsonObject u = req["contents"].to<JsonArray>().add<JsonObject>();
    u["role"] = "user"; u["parts"][0]["text"] = prompt;
    req["generationConfig"]["maxOutputTokens"] = 400;
    String body; serializeJson(req, body);
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
                 aiModel + ":generateContent?key=" + GEMINI_API_KEY;
    p = httpPostJSON(true, url, body, nullptr, nullptr, nullptr, nullptr, err);
    if (p == "") return "[error] " + err;
    if (deserializeJson(r, p)) return "[bad JSON]";
    if (r["error"]) return String("[api] ") + (const char*)(r["error"]["message"] | "");
    return String((const char*)(r["candidates"][0]["content"]["parts"][0]["text"] | "[no text]"));
  }
  if (aiProvider == "ollama") {
    if (!strlen(OLLAMA_HOST)) return "[set Ollama host in secrets.h]";
    JsonDocument req; req["model"] = aiModel; req["stream"] = false;
    JsonArray a = req["messages"].to<JsonArray>();
    JsonObject s = a.add<JsonObject>(); s["role"] = "system"; s["content"] = SYS_PROMPT;
    JsonObject u = a.add<JsonObject>(); u["role"] = "user"; u["content"] = prompt;
    String body; serializeJson(req, body);
    p = httpPostJSON(false, String("http://") + OLLAMA_HOST + "/api/chat", body,
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

static void sendPrompt(const String& prompt) {
  scrollLines = 0;
  String lbl = aiLabel() + ": ";
  addMsg("You: " + prompt, userColor);
  addMsg("...", C_DIM); draw();
  String reply = askAI(prompt);
  Serial.println(lbl + reply);
  if (!msgs.empty()) msgs.pop_back();       // remove "..."
  addMsg(lbl + reply, aiColor);
  // show YOUR message at the top of the new exchange (scroll down for long replies)
  setFont(chatFontIdx);
  std::vector<String> ex;
  wrapMsg("You: " + prompt, scrW - 4, ex);
  wrapMsg(lbl + reply, scrW - 4, ex);
  scrollLines = (int)ex.size() > lastRows ? (int)ex.size() - lastRows : 0;
  draw();
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
// Read one touch point. Returns raw controller coords in rx/ry; false if no touch.
static bool gtReadRaw(int& rx, int& ry) {
  if (!gtAddr) return false;
  Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom(gtAddr, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t st = Wire.read();
  bool touched = (st & 0x80) && (st & 0x0F);
  if (touched) {
    Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x50);
    Wire.endTransmission();
    Wire.requestFrom(gtAddr, (uint8_t)8);
    uint8_t b[8]; int i = 0; while (Wire.available() && i < 8) b[i++] = Wire.read();
    if (i >= 5) { rx = b[1] | (b[2] << 8); ry = b[3] | (b[4] << 8); }
  }
  Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
  Wire.endTransmission();
  return touched;
}
// Map raw touch -> screen coords for TFT rotation 1. FIRST GUESS — calibrated from serial logs.
static void gtMap(int rx, int ry, int& sx, int& sy) { sx = rx; sy = ry; }

static void connectWifi() {
  if (String(DEFAULT_WIFI_SSID).isEmpty()) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
}

// Branded boot splash: RoostOS wordmark + a little chick perched on an antenna.
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
  Serial.begin(115200); delay(300);
  Serial.printf("\n=== RoostOS Communicator APP %s ===\n", ROOST_COMM_VERSION);
  Wire.begin(I2C_SDA, I2C_SCL);
  gtProbe();   // detect GT911 touch controller

  tft.init(); tft.setRotation(1);
  scrW = tft.width(); scrH = tft.height();
  C_BG     = tft.color565(0x0d, 0x11, 0x17);
  C_PANEL  = tft.color565(0x15, 0x1d, 0x2c);
  C_INK    = tft.color565(0xee, 0xf2, 0xfb);
  C_DIM    = tft.color565(0x93, 0xa0, 0xc4);
  C_TEAL   = tft.color565(0x34, 0xe2, 0xc0);
  C_INDIGO = tft.color565(0x7c, 0x8c, 0xff);
  C_AMBER  = tft.color565(0xff, 0xbe, 0x4d);
  userColor = C_INDIGO; aiColor = C_TEAL;   // defaults
  loadCfg();                                 // override from saved settings (NVS)

  drawSplash();
  delay(splashMs);   // adjustable in settings

  int save = chatFontIdx; chatFontIdx = 0;   // boot in tiny so startup lines fit
  addMsg("Booting... connecting WiFi", C_DIM); draw();
  connectWifi();
  addMsg(WiFi.status() == WL_CONNECTED
    ? String("Ready to Roost! Type a message + Enter, or tap the menu.")
    : String("WiFi failed - check SSS-FAMILY"), C_TEAL);
  chatFontIdx = save;                         // swap to preferred size
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
  if (!strlen(OLLAMA_HOST)) return false;
  WiFiClient c; HTTPClient h;
  h.setConnectTimeout(3000); h.setTimeout(4000);
  if (!h.begin(c, String("http://") + OLLAMA_HOST + "/api/tags")) return false;
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
    msg = "ollama unreachable @ " + String(OLLAMA_HOST); return false;
  }
  aiProvider = p; aiModel = defaultModel(p); saveCfg();
  msg = "provider: " + aiProvider + " / " + aiModel; return true;
}

// command matches full name OR a >=3-char prefix (so /fon, /set, /col, /scr, /spl, /inp work)
static bool cmdIs(const String& tok, const char* full) {
  String f = full;
  return tok == f || (tok.length() >= 3 && f.startsWith(tok));
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
    if (p < 0) return "usage: /color user|ai <name>";
    String who = rest.substring(0, p), name = rest.substring(p + 1); name.trim();
    uint16_t col = namedColor(name);
    if (col == 0xFFFF) return "colors: teal indigo amber red green cyan magenta orange ink dim";
    if (who.startsWith("u")) userColor = col; else if (who.startsWith("a")) aiColor = col; else return "usage: /color user|ai <name>";
    saveCfg(); draw(); return who + " color set";
  }
  if (cmdIs(tok, "scroll")) { scrollStep = constrain(rest.toInt(), 1, 20); saveCfg(); return String("scroll rate: ") + scrollStep; }
  if (cmdIs(tok, "splash")) { splashMs = constrain(rest.toInt(), 0, 15000); saveCfg(); return String("splash: ") + splashMs + "ms"; }
  if (cmdIs(tok, "provider")) {
    String v = rest; v.toLowerCase();
    if (v.isEmpty()) return "providers: anthropic openai gemini ollama";
    String m; switchProvider(v, m); return m;
  }
  if (cmdIs(tok, "model")) { if (rest.length()) { aiModel = rest; saveCfg(); } return String("model: ") + aiModel; }
  return "";
}

void loop() {
  static bool clkPrev = true; static uint32_t clkT = 0, lastTouch = 0;
  uint32_t now = millis();

  // trackball CLICK (GPIO0): chat -> open settings; settings -> activate selection
  bool clk = digitalRead(TB_CLICK);
  if (!clk && clkPrev && now - clkT > 220) {
    clkT = now; Serial.println("[click]");
    if (uiMode == MODE_CHAT) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }
    else if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings(); }
    else activateSetting();
  }
  clkPrev = clk;

  // trackball ROLL up/down (scroll in chat; move selection in settings)
  bool u = digitalRead(TB_UP), d = digitalRead(TB_DOWN);
  if (uiMode == MODE_CHAT) {
    int before = scrollLines;
    if (!u && tbUpPrev && now - lastScroll > 90) { scrollLines += scrollStep; lastScroll = now; }
    if (!d && tbDnPrev && now - lastScroll > 90) { scrollLines -= scrollStep; lastScroll = now; }
    if (scrollLines < 0) scrollLines = 0;
    if (scrollLines != before) draw();
  } else if (uiMode == MODE_SETTINGS) {
    if (!u && tbUpPrev && now - lastScroll > 150) { selIdx = (selIdx - 1 + pageLen(setPage)) % pageLen(setPage); lastScroll = now; drawSettings(); }
    if (!d && tbDnPrev && now - lastScroll > 150) { selIdx = (selIdx + 1) % pageLen(setPage); lastScroll = now; drawSettings(); }
  }
  tbUpPrev = u; tbDnPrev = d;

  // TOUCH — log coords (for calibration) + hit-test
  if (now - lastTouch > 130) {
    int rx = 0, ry = 0;
    if (gtReadRaw(rx, ry)) {
      int sx, sy; gtMap(rx, ry, sx, sy);
      Serial.printf("[touch] raw=%d,%d screen=%d,%d mode=%d\n", rx, ry, sx, sy, uiMode);
      lastTouch = now;
      if (uiMode == MODE_CHAT) {
        if (sy < headerH && sx > scrW - 26) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }  // menu button
      } else if (uiMode == MODE_ABOUT) {
        uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings();
      } else {
        setFont(1); int lh = tft.fontHeight() + 6; int row = (sy - (headerH + 4)) / lh;
        if (row >= 0 && row < pageLen(setPage)) { selIdx = row; activateSetting(); }
      }
    }
  }

  // KEYBOARD
  uint8_t k = readKey();
  if (k) {
    if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; setPage = PG_SYSTEM; selIdx = 1; drawSettings(); }
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

  // SERIAL commands (ask/ip + settings nav for testing + config via applyCfgCmd)
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      String s = serialBuf; serialBuf = "";
      if (s.startsWith("ask ") || s.startsWith("claude "))
        sendPrompt(s.substring(s.indexOf(' ') + 1));
      else if (s == "ip")
        Serial.printf("ip=%s status=%d mode=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.status(), uiMode);
      else if (s == "click") { if (uiMode == MODE_CHAT) { setPage = PG_MAIN; uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); } else activateSetting(); Serial.printf("mode=%d sel=%d\n", uiMode, selIdx); }
      else if (s == "down")  { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx + 1) % pageLen(setPage); drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else if (s == "up")    { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx - 1 + pageLen(setPage)) % pageLen(setPage); drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else { String r = applyCfgCmd(s); Serial.println(r.length() ? r : "? (try: ask/font/color/scroll/splash/settings/ip)"); }
    } else serialBuf += c;
  }
  delay(8);
}
