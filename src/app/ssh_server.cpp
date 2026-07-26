// Real SSH server for the RoostOS Communicator (LibSSH-ESP32).
// Runs in its own FreeRTOS task with a large stack. On connect + password auth
// it opens an interactive shell; bytes are echoed + line-assembled here, and
// each complete line is marshaled to the main loop (which drives the shared
// chat/config shell). Output is queued back and written to the SSH channel.
//
// Host key: an ED25519 key is generated once and persisted (base64) in NVS, so
// no SPIFFS partition is needed. Gated by a Settings toggle (default off).
#include <Arduino.h>
#include <Preferences.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>
#include <libssh/server.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ---- cross-task marshaling (implemented here, called from app_main) ----
static QueueHandle_t inQ  = nullptr;   // completed input lines: SSH task -> main loop
static QueueHandle_t outQ = nullptr;   // output chunks: main loop -> SSH task
static const int SSH_LINE = 240;
static volatile bool sshTaskRunning = false;
static volatile bool sshClientActive = false;
static volatile bool sshWantStop = false;
static TaskHandle_t sshTask = nullptr;

// credentials + config, set by the app before start
static String sshUser = "roost", sshPass = "roostos";
extern "C" void sshSetCreds(const char* u, const char* p) { sshUser = u; sshPass = p; }

// main loop pops one pending input line (returns false if none)
bool sshPopLine(String& out) {
  if (!inQ) return false;
  char buf[SSH_LINE + 1];
  if (xQueueReceive(inQ, buf, 0) == pdTRUE) { out = buf; return true; }
  return false;
}
// main loop / shell sink: queue text out to the SSH client (translates \n->\r\n)
void sshQueueOut(const char* s) {
  if (!outQ) return;
  String t; for (const char* p = s; *p; p++) { if (*p == '\n') t += '\r'; t += *p; }
  int n = t.length(); int i = 0;
  while (i < n) {
    char buf[SSH_LINE + 1];
    int c = min(SSH_LINE, n - i); memcpy(buf, t.c_str() + i, c); buf[c] = 0;
    xQueueSend(outQ, buf, pdMS_TO_TICKS(200)); i += c;
  }
}
bool sshActive() { return sshClientActive; }
bool sshIsRunning() { return sshTaskRunning; }

// ---- host key in NVS ----
static ssh_key loadOrCreateHostKey() {
  Preferences p; p.begin("roostssh", false);
  String b64 = p.getString("hostkey", "");
  ssh_key key = nullptr;
  if (b64.length()) {
    if (ssh_pki_import_privkey_base64(b64.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK) {
      p.end(); return key;
    }
  }
  // generate a fresh ED25519 host key (fast) and persist it
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) { p.end(); return nullptr; }
  char* out = nullptr;
  if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &out) == SSH_OK && out) {
    p.putString("hostkey", out); ssh_string_free_char(out);
  }
  p.end();
  return key;
}

static int authPassword(const char* user, const char* pass) {
  return (sshUser == user && sshPass == pass) ? 1 : 0;
}

// authenticate loop (password only)
static bool doAuth(ssh_session session) {
  while (true) {
    ssh_message msg = ssh_message_get(session);
    if (!msg) return false;
    int type = ssh_message_type(msg), sub = ssh_message_subtype(msg);
    bool ok = false, done = false;
    if (type == SSH_REQUEST_SERVICE) {                 // accept the ssh-userauth service
      ssh_message_service_reply_success(msg);
    } else if (type == SSH_REQUEST_AUTH && sub == SSH_AUTH_METHOD_PASSWORD) {
      if (authPassword(ssh_message_auth_user(msg), ssh_message_auth_password(msg))) {
        ssh_message_auth_reply_success(msg, 0); ok = true; done = true;
      } else {
        ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
        ssh_message_reply_default(msg);
      }
    } else {                                           // none / other -> advertise password
      ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
      ssh_message_reply_default(msg);
    }
    ssh_message_free(msg);
    if (done) return ok;
  }
}

static ssh_channel waitChannel(ssh_session session) {
  ssh_channel chan = nullptr;
  while (!chan) {
    ssh_message msg = ssh_message_get(session);
    if (!msg) return nullptr;
    if (ssh_message_type(msg) == SSH_REQUEST_CHANNEL_OPEN &&
        ssh_message_subtype(msg) == SSH_CHANNEL_SESSION)
      chan = ssh_message_channel_request_open_reply_accept(msg);
    else ssh_message_reply_default(msg);
    ssh_message_free(msg);
  }
  // wait for a shell/pty request
  bool shell = false; int guard = 0;
  while (!shell && guard++ < 20) {
    ssh_message msg = ssh_message_get(session);
    if (!msg) break;
    if (ssh_message_type(msg) == SSH_REQUEST_CHANNEL &&
        (ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_SHELL ||
         ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_PTY)) {
      ssh_message_channel_request_reply_success(msg);
      if (ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_SHELL) shell = true;
    } else ssh_message_reply_default(msg);
    ssh_message_free(msg);
  }
  return chan;
}

static void serveClient(ssh_session session) {
  if (ssh_handle_key_exchange(session) != SSH_OK) return;
  if (!doAuth(session)) return;
  ssh_channel chan = waitChannel(session);
  if (!chan) return;
  sshClientActive = true;   // main loop detects this edge and prints the banner

  String line; char rb[128];
  while (!sshWantStop) {
    // drain queued output to the channel
    char ob[SSH_LINE + 1];
    while (xQueueReceive(outQ, ob, 0) == pdTRUE) ssh_channel_write(chan, ob, strlen(ob));
    // read input (non-blocking-ish)
    int n = ssh_channel_read_timeout(chan, rb, sizeof(rb), 0, 40);
    if (n == SSH_ERROR || (n == 0 && ssh_channel_is_eof(chan))) break;
    for (int i = 0; i < n; i++) {
      char c = rb[i];
      if (c == 3 || c == 4) { ssh_channel_write(chan, "\r\n", 2); goto done; }   // Ctrl-C/D
      if (c == '\r' || c == '\n') {
        ssh_channel_write(chan, "\r\n", 2);
        char lb[SSH_LINE + 1]; int c2 = min((int)line.length(), SSH_LINE);
        memcpy(lb, line.c_str(), c2); lb[c2] = 0;
        xQueueSend(inQ, lb, pdMS_TO_TICKS(200)); line = "";
      } else if (c == 8 || c == 127) {
        if (line.length()) { line.remove(line.length() - 1); ssh_channel_write(chan, "\b \b", 3); }
      } else if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
        line += c; ssh_channel_write(chan, &c, 1);   // echo
      }
    }
    if (n == 0) vTaskDelay(pdMS_TO_TICKS(5));
  }
done:
  sshClientActive = false;
  ssh_channel_close(chan); ssh_channel_free(chan);
}

static void sshTaskFn(void*) {
  libssh_begin();
  Serial.printf("[ssh] init, heap=%u\n", (unsigned)ESP.getFreeHeap());
  ssh_key hostkey = loadOrCreateHostKey();
  Serial.printf("[ssh] host key %s, heap=%u\n", hostkey ? "ready" : "FAILED", (unsigned)ESP.getFreeHeap());
  while (!sshWantStop) {
    ssh_bind sshbind = ssh_bind_new();
    if (!sshbind) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    int port = 22;
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &port);
    if (hostkey) ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY, hostkey);
    if (ssh_bind_listen(sshbind) < 0) { Serial.printf("[ssh] bind/listen failed: %s\n", ssh_get_error(sshbind)); ssh_bind_free(sshbind); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    Serial.printf("[ssh] listening on :22, heap=%u\n", (unsigned)ESP.getFreeHeap());
    while (!sshWantStop) {
      ssh_session session = ssh_new();
      if (ssh_bind_accept(sshbind, session) == SSH_OK) {
        serveClient(session);
        ssh_disconnect(session);
      }
      ssh_free(session);
    }
    ssh_bind_free(sshbind);
  }
  if (hostkey) ssh_key_free(hostkey);
  ssh_finalize();
  sshTaskRunning = false;
  vTaskDelete(nullptr);
}

void sshServerStart() {
  if (sshTaskRunning) return;
  if (!inQ)  inQ  = xQueueCreate(6, SSH_LINE + 1);
  if (!outQ) outQ = xQueueCreate(24, SSH_LINE + 1);
  sshWantStop = false; sshTaskRunning = true;
  xTaskCreatePinnedToCore(sshTaskFn, "sshd", 16384, nullptr, tskIDLE_PRIORITY + 2, &sshTask, 1);
}
void sshServerStop() {
  if (!sshTaskRunning) return;
  sshWantStop = true;   // task exits its loops and self-deletes
}
// Forget the persisted host key so a fresh one is generated on next start.
// (The key normally persists in NVS across reflashes, so clients don't warn.)
void sshRegenHostKey() {
  Preferences p; p.begin("roostssh", false); p.remove("hostkey"); p.end();
  if (sshTaskRunning) sshServerStop();   // main loop restarts it -> generates a new key
}
