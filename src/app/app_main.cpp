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
#include <vector>
#include "secrets.h"
#include "version.h"

// ---- pins ----
#define PIN_POWERON 10
#define PIN_BL      42
#define KB_ADDR     0x55
#define I2C_SDA     18
#define I2C_SCL     8
#define TB_UP       3    // trackball BOARD_TBOX_G01 -> scroll up
#define TB_DOWN     2    // trackball BOARD_TBOX_G02 -> scroll down

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
static void setFont(int idx) {
  const GFXfont* g = FONTS[idx].gfx;
  if (g) tft.setFreeFont(g); else { tft.setTextFont(1); tft.setTextSize(1); }
}

// ---- configurable state (future Settings) ----
static int chatFontIdx  = 2;   // message text size (default medium)
static int inputFontIdx = 1;   // input-box text size (default small)
static uint16_t userColor, aiColor;   // set in setup(); configurable

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
  while (i <= n) {
    if (i == n) { out.push_back(line); break; }
    int j = i; while (j < n && text[j] != ' ' && text[j] != '\n') j++;
    String word = text.substring(i, j);
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial) <= maxW) line = trial;
    else {
      if (line.length()) { out.push_back(line); line = ""; }
      if (tft.textWidth(word) > maxW) {
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
  String net = WiFi.status() == WL_CONNECTED
    ? (WiFi.SSID() + " " + WiFi.localIP().toString()) : String("WiFi down");
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(net, scrW - tft.textWidth(net) - 4, 4);

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
  // scrollback indicator
  if (scrollLines > 0) {
    tft.setTextFont(1); tft.setTextSize(1);
    tft.setTextColor(C_AMBER, C_BG);
    tft.drawString("^ more", scrW - 44, top);
  }

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
  // anchor: keep the user's message visible at the top of the new exchange
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
  pinMode(TB_UP, INPUT_PULLUP); pinMode(TB_DOWN, INPUT_PULLUP);
  Serial.begin(115200); delay(300);
  Serial.printf("\n=== RoostOS Communicator APP %s ===\n", ROOST_COMM_VERSION);
  Wire.begin(I2C_SDA, I2C_SCL);

  tft.init(); tft.setRotation(1);
  scrW = tft.width(); scrH = tft.height();
  C_BG     = tft.color565(0x0d, 0x11, 0x17);
  C_PANEL  = tft.color565(0x15, 0x1d, 0x2c);
  C_INK    = tft.color565(0xee, 0xf2, 0xfb);
  C_DIM    = tft.color565(0x93, 0xa0, 0xc4);
  C_TEAL   = tft.color565(0x34, 0xe2, 0xc0);
  C_INDIGO = tft.color565(0x7c, 0x8c, 0xff);
  C_AMBER  = tft.color565(0xff, 0xbe, 0x4d);
  userColor = C_INDIGO;   // configurable
  aiColor   = C_TEAL;     // configurable

  drawSplash();
  delay(1600);

  int save = chatFontIdx; chatFontIdx = 0;   // boot in tiny so startup lines fit
  addMsg("Booting... connecting WiFi", C_DIM); draw();
  connectWifi();
  addMsg(WiFi.status() == WL_CONNECTED
    ? "Connected " + WiFi.localIP().toString() + " - type + Enter"
    : "WiFi failed - check SSS-FAMILY", C_DIM);
  chatFontIdx = save;                         // swap to preferred size
  draw();
  Serial.printf("[wifi] status=%d ip=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  Serial.println("cmds: ask <t> | font <n> | inputfont <n> | color user|ai <name> | ip");
}

static String serialBuf;
static bool tbUpPrev = true, tbDnPrev = true;

static int fontArgToIdx(const String& arg, int cur) {
  if (arg == "tiny") return 0; if (arg == "small") return 1;
  if (arg == "medium") return 2; if (arg == "large") return 3;
  int pt = arg.toInt(); if (pt == 0) return cur;
  return pt <= 6 ? 0 : (pt <= 10 ? 1 : (pt <= 15 ? 2 : 3));
}

void loop() {
  // keyboard
  uint8_t k = readKey();
  if (k) {
    if (k == '\r' || k == '\n') { if (input.length()) { String p = input; input = ""; sendPrompt(p); } }
    else if (k == 8 || k == 127) { if (input.length()) { input.remove(input.length() - 1); draw(); } }
    else if (k >= 32 && k < 127) { input += (char)k; draw(); }
  }
  // trackball scroll (falling edge = one detent)
  bool u = digitalRead(TB_UP), d = digitalRead(TB_DOWN);
  if (!u && tbUpPrev) { scrollLines += 3; draw(); }
  if (!d && tbDnPrev) { scrollLines -= 3; if (scrollLines < 0) scrollLines = 0; draw(); }
  tbUpPrev = u; tbDnPrev = d;

  // serial dev commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      String s = serialBuf; serialBuf = "";
      if (s.startsWith("ask ") || s.startsWith("claude "))
        sendPrompt(s.substring(s.indexOf(' ') + 1));
      else if (s.startsWith("font ")) {
        chatFontIdx = fontArgToIdx(s.substring(5), chatFontIdx);
        Serial.printf("chat font=%s\n", FONTS[chatFontIdx].name); draw();
      } else if (s.startsWith("inputfont ")) {
        inputFontIdx = fontArgToIdx(s.substring(10), inputFontIdx);
        Serial.printf("input font=%s\n", FONTS[inputFontIdx].name); draw();
      } else if (s.startsWith("color ")) {
        int sp = s.indexOf(' ', 6);
        String who = s.substring(6, sp), name = s.substring(sp + 1); name.trim();
        uint16_t col = namedColor(name);
        if (col == 0xFFFF) Serial.println("colors: teal indigo amber red green cyan magenta orange ink dim");
        else { if (who == "user") userColor = col; else if (who == "ai") aiColor = col;
               Serial.printf("%s color set\n", who.c_str()); draw(); }
      } else if (s == "ip")
        Serial.printf("ip=%s status=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.status());
    } else serialBuf += c;
  }
  delay(8);
}
