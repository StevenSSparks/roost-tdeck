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
#include "secrets.h"
#include "version.h"

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
#include <vector>
struct Line { String text; uint16_t color; };
static std::vector<Line> lines;     // wrapped display lines
static String input;                 // current input line
static const int CHARW = 6, CHARH = 8;   // GLCD font @ size 1
static int scrW, scrH, cols, headerH, inputH, chatRows;

static void wrapAppend(const String& s, uint16_t color) {
  // word-wrap s into display lines of <= cols chars
  String cur;
  int i = 0;
  while (i < (int)s.length()) {
    char c = s[i++];
    if (c == '\n') { lines.push_back({cur, color}); cur = ""; continue; }
    cur += c;
    if ((int)cur.length() >= cols) { lines.push_back({cur, color}); cur = ""; }
  }
  lines.push_back({cur, color});
  // cap history
  while (lines.size() > 400) lines.erase(lines.begin());
}

static void draw() {
  tft.fillScreen(C_BG);
  // header
  tft.fillRect(0, 0, scrW, headerH, C_PANEL);
  tft.setTextSize(1);
  tft.setTextColor(C_TEAL, C_PANEL);
  tft.setCursor(4, 4);
  tft.print("RoostOS");
  tft.setTextColor(C_INK, C_PANEL);
  tft.print(" Communicator");
  tft.setTextColor(C_DIM, C_PANEL);
  String net = WiFi.status() == WL_CONNECTED
    ? (WiFi.SSID() + " " + WiFi.localIP().toString()) : String("WiFi down");
  tft.setCursor(scrW - (int)net.length() * CHARW - 4, 4);
  tft.print(net);

  // chat area (last chatRows lines)
  int top = headerH + 2;
  int start = (int)lines.size() > chatRows ? (int)lines.size() - chatRows : 0;
  int y = top;
  for (int i = start; i < (int)lines.size(); i++) {
    tft.setTextColor(lines[i].color, C_BG);
    tft.setCursor(2, y);
    tft.print(lines[i].text);
    y += CHARH + 1;
  }

  // input line
  int iy = scrH - inputH;
  tft.fillRect(0, iy, scrW, inputH, C_PANEL);
  tft.setTextColor(C_AMBER, C_PANEL);
  tft.setCursor(2, iy + 3);
  tft.print("> ");
  tft.setTextColor(C_INK, C_PANEL);
  String shown = input;
  if ((int)shown.length() > cols - 2) shown = shown.substring(shown.length() - (cols - 2));
  tft.print(shown);
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
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject m = msgs.add<JsonObject>(); m["role"] = "user"; m["content"] = prompt;
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
  wrapAppend("You: " + prompt, C_INDIGO);
  wrapAppend("...", C_DIM); draw();
  String reply = askClaude(prompt);
  Serial.print("Haiku: "); Serial.println(reply);   // dev echo (also rendered on screen)
  if (!lines.empty()) lines.pop_back();  // remove "..."
  wrapAppend("Haiku: " + reply, C_TEAL);
  wrapAppend("", C_DIM);
  draw();
}

// ---- keyboard ----
static uint8_t readKey() {
  Wire.requestFrom(KB_ADDR, 1);
  if (Wire.available()) { uint8_t k = Wire.read(); return k; }
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
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);   // backlight on
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== RoostOS Communicator APP %s ===\n", ROOST_COMM_VERSION);

  Wire.begin(I2C_SDA, I2C_SCL);

  tft.init();
  tft.setRotation(1);           // landscape 320x240
  scrW = tft.width(); scrH = tft.height();
  C_BG     = tft.color565(0x0d, 0x11, 0x17);
  C_PANEL  = tft.color565(0x15, 0x1d, 0x2c);
  C_INK    = tft.color565(0xee, 0xf2, 0xfb);
  C_DIM    = tft.color565(0x93, 0xa0, 0xc4);
  C_TEAL   = tft.color565(0x34, 0xe2, 0xc0);
  C_INDIGO = tft.color565(0x7c, 0x8c, 0xff);
  C_AMBER  = tft.color565(0xff, 0xbe, 0x4d);
  cols = scrW / CHARW;
  headerH = CHARH + 6; inputH = CHARH + 6;
  chatRows = (scrH - headerH - inputH - 4) / (CHARH + 1);

  Serial.printf("[tft] %dx%d cols=%d chatRows=%d\n", scrW, scrH, cols, chatRows);
  wrapAppend("Booting… connecting WiFi", C_DIM);
  draw();

  connectWifi();
  wrapAppend(WiFi.status() == WL_CONNECTED
    ? "Connected: " + WiFi.localIP().toString() + "  — type a message + Enter"
    : "WiFi failed — check SSS-FAMILY", C_DIM);
  draw();
  Serial.printf("[wifi] status=%d ip=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  Serial.println("serial: type 'ask <text>' too");
}

static String serialBuf;

void loop() {
  // keyboard
  uint8_t k = readKey();
  if (k) {
    if (k == '\r' || k == '\n') {
      if (input.length()) { String p = input; input = ""; sendPrompt(p); }
    } else if (k == 8 || k == 127) {
      if (input.length()) { input.remove(input.length() - 1); draw(); }
    } else if (k >= 32 && k < 127) {
      input += (char)k; draw();
    }
  }
  // serial ask (dev bridge)
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.startsWith("ask ") || serialBuf.startsWith("claude "))
        sendPrompt(serialBuf.substring(serialBuf.indexOf(' ') + 1));
      else if (serialBuf == "ip")
        Serial.printf("ip=%s status=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.status());
      serialBuf = "";
    } else serialBuf += c;
  }
  delay(10);
}
