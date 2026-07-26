// RoostOS Communicator — SMOKE TEST firmware (not the real app).
// Proves the risky integration early: board power, WiFi (SSS-MAIN), TLS, and a
// live Claude Haiku call. Command shell over BOTH USB serial and TCP port 23
// (telnet/nc) — the same surface a real SSH shell will expose later.
//
// Commands:  claude <text>   -> ask Haiku, print reply
//            ip | status     -> network + heap info
//            help
//
// Built by [env:smoke]. Uses include/secrets.h (DEV_SECRETS).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "version.h"

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

static const char* ANTHROPIC_URL = "https://api.anthropic.com/v1/messages";
static const char* MODEL = "claude-haiku-4-5";

static WiFiServer tcpServer(23);
static WiFiClient tcpClient;

static void connectWifi();  // fwd decl (defined below; used by the `wifi` command)

// Ask Claude Haiku a single-turn question; return the assistant text or an error.
static String askClaude(const String& prompt) {
  if (WiFi.status() != WL_CONNECTED) return "[error] WiFi not connected";

  WiFiClientSecure tls;
  tls.setInsecure();  // smoke test: skip cert validation (real app pins/bundles CA)
  HTTPClient https;
  https.setTimeout(20000);
  if (!https.begin(tls, ANTHROPIC_URL)) return "[error] https.begin failed";
  https.addHeader("content-type", "application/json");
  https.addHeader("x-api-key", ANTHROPIC_API_KEY);
  https.addHeader("anthropic-version", "2023-06-01");

  JsonDocument req;
  req["model"] = MODEL;
  req["max_tokens"] = 256;
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject m = msgs.add<JsonObject>();
  m["role"] = "user";
  m["content"] = prompt;
  String body;
  serializeJson(req, body);

  int code = https.POST(body);
  String payload = https.getString();
  https.end();

  if (code <= 0) return String("[error] POST failed: ") + https.errorToString(code);

  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, payload);
  if (err) return String("[error] bad JSON (HTTP ") + code + "): " + payload.substring(0, 160);

  if (resp["type"] == "error")
    return String("[api error ") + code + "] " + (const char*)(resp["error"]["type"] | "unknown") +
           ": " + (const char*)(resp["error"]["message"] | "");

  String text;
  for (JsonObject block : resp["content"].as<JsonArray>())
    if (block["type"] == "text") text += (const char*)(block["text"] | "");
  long in = resp["usage"]["input_tokens"] | 0;
  long out = resp["usage"]["output_tokens"] | 0;
  if (text.isEmpty()) text = "[no text in response] " + payload.substring(0, 160);
  return text + "\n  (tokens in=" + in + " out=" + out + ")";
}

static void handleCommand(const String& lineIn, Print& out) {
  String line = lineIn; line.trim();
  if (line.isEmpty()) return;
  if (line == "help") {
    out.println("commands: ask <text> | claude <text> | wifi | scan | ip | status | help");
  } else if (line == "ip" || line == "status") {
    out.printf("RoostOS Comm %s (smoke)\n", ROOST_COMM_VERSION);
    out.printf("compiled SSID='%s' (passlen=%d)\n", DEFAULT_WIFI_SSID, (int)strlen(DEFAULT_WIFI_PASS));
    out.printf("WiFi: %s  status=%d  SSID=%s  IP=%s  RSSI=%d\n",
               WiFi.status() == WL_CONNECTED ? "up" : "down", (int)WiFi.status(),
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    out.printf("heap=%u  psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
  } else if (line == "wifi") {
    out.println("… (re)connecting …");
    connectWifi();
    out.printf("WiFi status=%d  IP=%s\n", (int)WiFi.status(), WiFi.localIP().toString().c_str());
  } else if (line == "scan") {
    out.println("scanning 2.4GHz …");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(200);
    int n = WiFi.scanNetworks(false, true);  // blocking, show hidden
    if (n < 0) { delay(500); n = WiFi.scanNetworks(false, true); }  // retry once
    for (int i = 0; i < n; i++)
      out.printf("  %-24s rssi=%d ch=%d %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                 WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "lock");
    out.printf("(%d networks)\n", n);
    WiFi.scanDelete();
  } else if (line.startsWith("join ")) {
    String ssid = line.substring(5); ssid.trim();
    out.printf("joining '%s' with compiled password …\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), DEFAULT_WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); out.print("."); }
    out.println();
    out.printf("status=%d  IP=%s  RSSI=%d\n", (int)WiFi.status(),
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (line.startsWith("join2 ")) {
    // join2 <ssid> <password>  — test any network live (password not stored/committed)
    String rest = line.substring(6); rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 1) { out.println("usage: join2 <ssid> <password>"); return; }
    String ssid = rest.substring(0, sp);
    String pass = rest.substring(sp + 1);
    out.printf("joining '%s' (passlen=%d) …\n", ssid.c_str(), pass.length());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); out.print("."); }
    out.println();
    out.printf("status=%d  IP=%s  RSSI=%d\n", (int)WiFi.status(),
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (WiFi.status() == WL_CONNECTED) out.println("CONNECTED — try: ask hello");
  } else if (line.startsWith("claude ") || line.startsWith("ask ")) {
    String prompt = line.substring(line.indexOf(' ') + 1);
    out.println("… asking Haiku …");
    out.println(askClaude(prompt));
  } else {
    out.println("? unknown command. try: help");
  }
}

static void connectWifi() {
  if (String(DEFAULT_WIFI_SSID).isEmpty()) {
    Serial.println("[wifi] no DEFAULT_WIFI_SSID compiled in");
    return;
  }
  Serial.printf("[wifi] connecting to %s …\n", DEFAULT_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[wifi] connected  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.println("[wifi] FAILED to connect");
}

void setup() {
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);           // board power-enable (GPIO10) — critical
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== RoostOS Communicator SMOKE %s ===\n", ROOST_COMM_VERSION);
  connectWifi();
  tcpServer.begin();
  Serial.println("[tcp] shell on port 23 (nc <ip> 23)");
  Serial.println("type: help");
}

static String serialBuf, tcpBuf;

void loop() {
  // Serial line assembly
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (serialBuf.length()) { handleCommand(serialBuf, Serial); serialBuf = ""; } }
    else serialBuf += c;
  }
  // TCP client
  if (!tcpClient || !tcpClient.connected()) {
    WiFiClient nc = tcpServer.available();
    if (nc) { tcpClient = nc; tcpClient.println("RoostOS Comm smoke shell — type: help"); }
  }
  if (tcpClient && tcpClient.connected()) {
    while (tcpClient.available()) {
      char c = tcpClient.read();
      if (c == '\n' || c == '\r') { if (tcpBuf.length()) { handleCommand(tcpBuf, tcpClient); tcpBuf = ""; } }
      else if (c >= 32) tcpBuf += c;
    }
  }
  delay(5);
}
