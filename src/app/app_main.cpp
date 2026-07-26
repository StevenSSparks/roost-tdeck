// RoostOS Communicator — on-screen app (first display build).
// ST7789 320x240 via TFT_eSPI, physical keyboard (I2C 0x55), WiFi + Claude Haiku.
// On-screen chat: type on the keyboard, see Haiku's reply. Serial `ask` still works.
// Built by [env:app]. Uses include/secrets.h (DEV_SECRETS).

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

// Chat font choices: index 0=tiny GLCD, 1..3 = smooth FreeSans small/med/large.
// Proportional (variable-width) fonts wrapped by pixel width.
struct FontChoice { const GFXfont* gfx; const char* name; };  // gfx==nullptr => GLCD
static const FontChoice FONTS[] = {
  { nullptr,           "tiny"   },   // 0: GLCD 6x8
  { &FreeSans9pt7b,    "small"  },   // 1
  { &FreeSans12pt7b,   "medium" },   // 2
  { &FreeSans18pt7b,   "large"  },   // 3
};
static const int NFONTS = 4;

// ---- T-Deck Plus pins (HARDWARE.md) ----
#define PIN_POWERON 10
#define PIN_BL      42
#define KB_ADDR     0x55
#define I2C_SDA     18
#define I2C_SCL     8

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

static const char* ANTHROPIC_URL = "https://api.anthropic.com/v1/messages";
static const char* MODEL = "claude-haiku-4-5";

// ---- RoostOS theme (RGB565 via tft.color565) ----
TFT_eSPI tft = TFT_eSPI();
static uint16_t C_BG, C_PANEL, C_INK, C_DIM, C_TEAL, C_INDIGO, C_AMBER;

// ---- chat state ----
struct Msg { String text; uint16_t color; };
static std::vector<Msg> msgs;        // raw (unwrapped) messages
static String input;                 // current input line
static int scrW, scrH;
static const int headerH = 15;
static int fontIdx = 1;              // index into FONTS[]; default "small" FreeSans

static void addMsg(const String& t, uint16_t color) {
  msgs.push_back({t, color});
  while (msgs.size() > 200) msgs.erase(msgs.begin());
}

static void setChatFont() {
  const GFXfont* g = FONTS[fontIdx].gfx;
  if (g) tft.setFreeFont(g);
  else { tft.setTextFont(1); tft.setTextSize(1); }
}

// Wrap one message to fit maxW pixels using the CURRENT font (proportional-safe).
static void wrapMsg(const String& text, int maxW, std::vector<String>& out) {
  String line;
  int i = 0, n = text.length();
  while (i <= n) {
    if (i == n) { out.push_back(line); break; }
    int j = i;
    while (j < n && text[j] != ' ' && text[j] != '\n') j++;
    String word = text.substring(i, j);
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial) <= maxW) {
      line = trial;
    } else {
      if (line.length()) { out.push_back(line); line = ""; }
      if (tft.textWidth(word) > maxW) {            // single word too long: hard-break
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

  // header — compact GLCD
  tft.setTextFont(1); tft.setTextSize(1);
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextColor(C_TEAL, C_PANEL); tft.drawString("RoostOS", 4, 4);
  int rx = 4 + tft.textWidth("RoostOS");
  tft.setTextColor(C_INK, C_PANEL); tft.drawString(" Communicator", rx, 4);
  String net = WiFi.status() == WL_CONNECTED
    ? (WiFi.SSID() + " " + WiFi.localIP().toString()) : String("WiFi down");
  tft.setTextColor(C_DIM, C_PANEL);
  tft.drawString(net, scrW - tft.textWidth(net) - 4, 4);

  // chat font metrics
  setChatFont();
  int lh = tft.fontHeight() + 2;
  int inputH = tft.fontHeight() + 6;
  int top = headerH + 3;
  int inputY = scrH - inputH;
  int maxW = scrW - 4;

  // wrap every message
  std::vector<std::pair<String, uint16_t>> wl;
  for (auto& m : msgs) {
    std::vector<String> ls; wrapMsg(m.text, maxW, ls);
    for (auto& s : ls) wl.push_back({s, m.color});
  }
  int rows = (inputY - top) / lh;
  int start = (int)wl.size() > rows ? (int)wl.size() - rows : 0;
  int y = top;
  for (int i = start; i < (int)wl.size(); i++) {
    tft.setTextColor(wl[i].second, C_BG);
    tft.drawString(wl[i].first, 2, y);
    y += lh;
  }

  // input line
  tft.fillRect(0, inputY, scrW, inputH, C_PANEL);
  setChatFont();
  tft.setTextColor(C_AMBER, C_PANEL); tft.drawString("> ", 2, inputY + 2);
  int pw = tft.textWidth("> ") + 2;
  tft.setTextColor(C_INK, C_PANEL);
  String shown = input;
  while (shown.length() && tft.textWidth(shown) > maxW - pw) shown = shown.substring(1);
  tft.drawString(shown, 2 + pw, inputY + 2);
}

// ---- Claude call (proven inline path) ----
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
    "no bullet symbols, no emoji, no special/unicode characters. Keep replies short "
    "and to the point.";
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
  addMsg("You: " + prompt, C_INDIGO);
  addMsg("...", C_DIM); draw();
  String reply = askClaude(prompt);
  Serial.print("Haiku: "); Serial.println(reply);   // dev echo (also on screen)
  if (!msgs.empty()) msgs.pop_back();                // remove "..."
  addMsg("Haiku: " + reply, C_TEAL);
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

void setup() {
  pinMode(PIN_POWERON, OUTPUT); digitalWrite(PIN_POWERON, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
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
  Serial.printf("[tft] %dx%d font=%s\n", scrW, scrH, FONTS[fontIdx].name);

  fontIdx = 0;   // boot with tiny font so startup messages fit
  addMsg("Booting... connecting WiFi", C_DIM); draw();
  connectWifi();
  addMsg(WiFi.status() == WL_CONNECTED
    ? "Connected " + WiFi.localIP().toString() + " - type + Enter"
    : "WiFi failed - check SSS-FAMILY", C_DIM);
  fontIdx = 2;   // swap to preferred font (medium; will come from Settings/NVS later)
  draw();
  Serial.printf("[wifi] status=%d ip=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  Serial.println("serial: 'ask <text>' | 'font <1-4>' | 'ip'");
}

static String serialBuf;

void loop() {
  uint8_t k = readKey();
  if (k) {
    if (k == '\r' || k == '\n') { if (input.length()) { String p = input; input = ""; sendPrompt(p); } }
    else if (k == 8 || k == 127) { if (input.length()) { input.remove(input.length() - 1); draw(); } }
    else if (k >= 32 && k < 127) { input += (char)k; draw(); }
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.startsWith("ask ") || serialBuf.startsWith("claude "))
        sendPrompt(serialBuf.substring(serialBuf.indexOf(' ') + 1));
      else if (serialBuf.startsWith("font ")) {
        String arg = serialBuf.substring(5); arg.trim();
        int idx = fontIdx;
        if (arg == "tiny") idx = 0; else if (arg == "small") idx = 1;
        else if (arg == "medium") idx = 2; else if (arg == "large") idx = 3;
        else { int pt = arg.toInt(); idx = pt <= 6 ? 0 : (pt <= 10 ? 1 : (pt <= 15 ? 2 : 3)); }
        fontIdx = idx;
        Serial.printf("font=%s (idx %d)  [options: tiny/small(9)/medium(12)/large(18)]\n",
                      FONTS[fontIdx].name, fontIdx);
        draw();
      } else if (serialBuf == "ip")
        Serial.printf("ip=%s status=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.status());
      serialBuf = "";
    } else serialBuf += c;
  }
  delay(10);
}
