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

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

static const char* ANTHROPIC_URL = "https://api.anthropic.com/v1/messages";
static const char* MODEL = "claude-haiku-4-5";

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

static void saveCfg() {
  prefs.begin("roostcomm", false);
  prefs.putInt("chatFont", chatFontIdx);
  prefs.putInt("inputFont", inputFontIdx);
  prefs.putUShort("userCol", userColor);
  prefs.putUShort("aiCol", aiColor);
  prefs.putInt("splashMs", splashMs);
  prefs.putInt("scrollStep", scrollStep);
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

// ---- Settings screen (trackball-navigated) ----
static const char* SET_ITEMS[] = {
  "Back to chat", "Chat font", "Input font", "You color", "Haiku color", "Scroll rate", "Splash", "About"
};
static const int N_SET = 8;

static String setValue(int i) {
  switch (i) {
    case 1: return FONTS[chatFontIdx].name;
    case 2: return FONTS[inputFontIdx].name;
    case 3: return PAL_NAMES[palIndexOf(userColor)];
    case 4: return PAL_NAMES[palIndexOf(aiColor)];
    case 5: return String(scrollStep);
    case 6: return String(splashMs / 1000.0, 1) + "s";
    default: return "";
  }
}

static void drawSettings() {
  tft.fillScreen(C_BG); tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(" Settings", 4 + tft.textWidth("RoostOS"), 4);
  setFont(1);   // small
  int lh = tft.fontHeight() + 6, y = headerH + 6;
  for (int i = 0; i < N_SET; i++) {
    if (i == selIdx) { tft.fillRect(0, y - 2, scrW, lh, C_PANEL); }
    uint16_t c = (i == selIdx) ? C_AMBER : C_INK;
    if (i == 3) c = (i == selIdx) ? C_AMBER : userColor;
    if (i == 4) c = (i == selIdx) ? C_AMBER : aiColor;
    tft.setTextColor(c, (i == selIdx) ? C_PANEL : C_BG);
    tft.drawString(SET_ITEMS[i], 6, y);
    String v = setValue(i);
    if (v.length()) { tft.setTextColor((i == selIdx) ? C_INK : C_DIM, (i == selIdx) ? C_PANEL : C_BG);
                      tft.drawString(v, scrW - tft.textWidth(v) - 8, y); }
    y += lh;
  }
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextColor(C_DIM, C_BG);
  tft.drawString("ball: roll=move  click=change  (or type /set)", 4, scrH - 12);
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

static void activateSetting() {
  if (selIdx == 7) { uiMode = MODE_ABOUT; drawAbout(); return; }   // About
  switch (selIdx) {
    case 0: uiMode = MODE_CHAT; draw(); return;
    case 1: chatFontIdx = (chatFontIdx + 1) % NFONTS; break;
    case 2: inputFontIdx = (inputFontIdx + 1) % NFONTS; break;
    case 3: userColor = namedColor(PAL_NAMES[(palIndexOf(userColor) + 1) % NPAL]); break;
    case 4: aiColor   = namedColor(PAL_NAMES[(palIndexOf(aiColor) + 1) % NPAL]); break;
    case 5: scrollStep = scrollStep >= 8 ? 1 : scrollStep + 1; break;
    case 6: { int opts[] = {0, 1500, 3000, 5000}; int cur = 0;
              for (int k = 0; k < 4; k++) if (opts[k] == splashMs) cur = k;
              splashMs = opts[(cur + 1) % 4]; break; }
  }
  saveCfg(); drawSettings();
}

static String askClaude(const String& prompt) {
  if (WiFi.status() != WL_CONNECTED) return "[error] WiFi not connected";
  WiFiClientSecure tls; tls.setInsecure();
  HTTPClient https; https.setTimeout(20000);
  if (!https.begin(tls, ANTHROPIC_URL)) return "[error] https.begin failed";
  https.addHeader("content-type", "application/json");
  https.addHeader("x-api-key", ANTHROPIC_API_KEY);
  https.addHeader("anthropic-version", "2023-06-01");
  JsonDocument req;
  req["model"] = MODEL; req["max_tokens"] = 400;
  req["system"] =
    "You are RoostOS, a friendly assistant on a small handheld device with a tiny "
    "pixel-font screen. Reply in plain ASCII text only: no markdown, no headings, "
    "no bullet symbols, no emoji, no special/unicode characters. Keep replies short.";
  JsonArray a = req["messages"].to<JsonArray>();
  JsonObject m = a.add<JsonObject>(); m["role"] = "user"; m["content"] = prompt;
  String body; serializeJson(req, body);
  int code = https.POST(body);
  String payload = https.getString(); https.end();
  if (code <= 0) return String("[error] POST failed: ") + https.errorToString(code);
  JsonDocument resp;
  if (deserializeJson(resp, payload)) return String("[error] bad JSON HTTP ") + code;
  if (resp["type"] == "error")
    return String("[api ") + (const char*)(resp["error"]["type"] | "error") + "] " +
           (const char*)(resp["error"]["message"] | "");
  String text;
  for (JsonObject b : resp["content"].as<JsonArray>())
    if (b["type"] == "text") text += (const char*)(b["text"] | "");
  return text.isEmpty() ? String("[no text]") : text;
}

static void sendPrompt(const String& prompt) {
  scrollLines = 0;
  addMsg("You: " + prompt, userColor);
  addMsg("...", C_DIM); draw();
  String reply = askClaude(prompt);
  Serial.print("Haiku: "); Serial.println(reply);
  if (!msgs.empty()) msgs.pop_back();       // remove "..."
  addMsg("Haiku: " + reply, aiColor);
  // show YOUR message at the top of the new exchange (scroll down for long replies)
  setFont(chatFontIdx);
  std::vector<String> ex;
  wrapMsg("You: " + prompt, scrW - 4, ex);
  wrapMsg("Haiku: " + reply, scrW - 4, ex);
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
  if (cmdIs(tok, "settings")) { uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); return " "; }
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
  return "";
}

void loop() {
  static bool clkPrev = true; static uint32_t clkT = 0, lastTouch = 0;
  uint32_t now = millis();

  // trackball CLICK (GPIO0): chat -> open settings; settings -> activate selection
  bool clk = digitalRead(TB_CLICK);
  if (!clk && clkPrev && now - clkT > 220) {
    clkT = now; Serial.println("[click]");
    if (uiMode == MODE_CHAT) { uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }
    else if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; drawSettings(); }
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
    if (!u && tbUpPrev && now - lastScroll > 150) { selIdx = (selIdx - 1 + N_SET) % N_SET; lastScroll = now; drawSettings(); }
    if (!d && tbDnPrev && now - lastScroll > 150) { selIdx = (selIdx + 1) % N_SET; lastScroll = now; drawSettings(); }
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
        if (sy < headerH && sx > scrW - 26) { uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); }  // menu button
      } else if (uiMode == MODE_ABOUT) {
        uiMode = MODE_SETTINGS; drawSettings();
      } else {
        setFont(1); int lh = tft.fontHeight() + 6; int row = (sy - (headerH + 4)) / lh;
        if (row >= 0 && row < N_SET) { selIdx = row; activateSetting(); }
      }
    }
  }

  // KEYBOARD
  uint8_t k = readKey();
  if (k) {
    if (uiMode == MODE_ABOUT) { uiMode = MODE_SETTINGS; drawSettings(); }
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
      else if (s == "click") { if (uiMode == MODE_CHAT) { uiMode = MODE_SETTINGS; selIdx = 0; drawSettings(); } else activateSetting(); Serial.printf("mode=%d sel=%d\n", uiMode, selIdx); }
      else if (s == "down")  { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx + 1) % N_SET; drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else if (s == "up")    { if (uiMode == MODE_SETTINGS) { selIdx = (selIdx - 1 + N_SET) % N_SET; drawSettings(); } Serial.printf("sel=%d\n", selIdx); }
      else { String r = applyCfgCmd(s); Serial.println(r.length() ? r : "? (try: ask/font/color/scroll/splash/settings/ip)"); }
    } else serialBuf += c;
  }
  delay(8);
}
