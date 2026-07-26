# RoostOS Communicator M1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship an expanded Milestone 1 of the RoostOS Communicator firmware for the LILYGO T-Deck Plus: hardware bringup, any-WiFi (captive-portal + MAC-clone), Claude chat with on-device tool use (GPS, map, battery, tone), stats, themes, and keychain-based key provisioning.

**Architecture:** Arduino-ESP32 firmware built with PlatformIO. Pure logic (settings serialization, chat history, Claude request/response + tool-loop assembly, Geoapify URL, usage math) lives in platform-independent modules unit-tested with the PlatformIO `native` environment. Hardware, networking, and LVGL UI wire those modules to the device and are verified on-device. Claude drives the device via a bounded agentic tool loop over the Anthropic Messages API.

**Tech Stack:** PlatformIO, Arduino-ESP32 (ESP32-S3), LVGL, ArduinoJson, TJpg_Decoder, WiFiClientSecure/HTTPClient, Preferences (NVS), Unity (native tests).

## Global Constraints

- Board: **ESP32-S3**, PSRAM **OPI** enabled, 16 MB flash, monitor 115200. (from spec §2/§3)
- Power-enable **GPIO 10** driven HIGH early in `setup()` before using peripherals. (§2)
- Keyboard on **I2C address 0x55**, 1 byte = keycode. (§2)
- UI toolkit is **LVGL**; all screens use the RoostOS theme. (§3/§5)
- Default model string **`claude-haiku-4-5`**; default `max_tokens` **512**; non-streaming. (§7)
- Anthropic endpoint `https://api.anthropic.com/v1/messages`, headers `x-api-key`, `anthropic-version: 2023-06-01`, `content-type: application/json`. (§7)
- Tool-loop iteration cap default **4** (configurable). (§7)
- **No secret is ever committed.** `secrets.h`, `.pio/`, `build/`, `*.bin` are gitignored. Keys come from the macOS login keychain items `anthropic_api_key_sparkshost` and `gtoapify_key_sfehost`. (§11)
- Brand palette (RoostOS default): bg `#0d1117`, bg2 `#0a0e15`, panel `#151d2c`, panel2 `#1b2436`, edge `#243049`, ink `#eef2fb`, dim `#93a0c4`, teal `#34e2c0`, indigo `#7c8cff`, amber `#ffbe4d`. (§5)
- Pin numbers and peripheral init sequences are **taken from LILYGO's official T-Deck repo** (Task 9), never guessed. (§2)
- Every task ends green and gets its own commit. `native` tests must pass before hardware tasks are declared done.

---

## Phase 0 — Scaffold & provisioning

### Task 1: PlatformIO project scaffold + native test env + remote repo

**Files:**
- Create: `platformio.ini`
- Create: `src/main.cpp` (temporary stub)
- Create: `README.md`
- Create: `test/native/test_smoke/test_smoke.cpp`
- Modify: `.gitignore` (already present; extend)

**Interfaces:**
- Produces: two PlatformIO environments — `esp32s3` (device) and `native` (host unit tests) — that all later tasks build against.

- [ ] **Step 1: Write `platformio.ini` with both environments**

```ini
[platformio]
default_envs = esp32s3

[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.flash_size = 16MB
board_build.arduino.memory_type = qio_opi   ; enable OPI PSRAM
build_flags =
  -DBOARD_HAS_PSRAM
  -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
  bblanchon/ArduinoJson@^7
  lvgl/lvgl@^8.4
  bodmer/TJpg_Decoder@^1.1
extra_scripts = pre:scripts/gen_secrets.py

[env:native]
platform = native
test_framework = unity
build_flags = -DNATIVE_TEST -std=gnu++17
lib_deps =
  bblanchon/ArduinoJson@^7
```

- [ ] **Step 2: Write a temporary `src/main.cpp` stub**

```cpp
#include <Arduino.h>
void setup() {}
void loop() {}
```

- [ ] **Step 3: Write the smoke test**

```cpp
#include <unity.h>
void test_sanity() { TEST_ASSERT_EQUAL(4, 2 + 2); }
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sanity);
  return UNITY_END();
}
```

- [ ] **Step 4: Run the native test, verify it passes**

Run: `pio test -e native`
Expected: 1 test, PASS. (Creating `scripts/gen_secrets.py` happens in Task 2; if the `esp32s3` build is attempted before then it will fail on the missing extra_script — that's expected and covered next.)

- [ ] **Step 5: Create the public GitHub remote and push**

```bash
cd ~/dev/tdeck-claude-communicator
gh repo create StevenSSparks/roost-tdeck --public --source=. --remote=origin \
  --description "RoostOS Communicator — LILYGO T-Deck Plus handheld: any-WiFi Claude chat with on-device tools (GPS, maps, battery, tones)."
git add -A && git commit -m "chore: PlatformIO scaffold + native test env"
git push -u origin main
```

### Task 2: Keychain-based secrets generation

**Files:**
- Create: `scripts/gen_secrets.py`
- Create: `include/secrets.example.h`

**Interfaces:**
- Produces: generated `include/secrets.h` (gitignored) defining `ANTHROPIC_API_KEY` and `GEOAPIFY_KEY` when built with `-DDEV_SECRETS`; a build-time error when a keychain item is missing.

- [ ] **Step 1: Write `include/secrets.example.h`**

```cpp
#pragma once
// This file is a template. The real include/secrets.h is generated at build
// time from the macOS keychain by scripts/gen_secrets.py and is gitignored.
#define ANTHROPIC_API_KEY ""
#define GEOAPIFY_KEY ""
```

- [ ] **Step 2: Write `scripts/gen_secrets.py`**

```python
Import("env")
import subprocess, os, sys

ITEMS = {
    "ANTHROPIC_API_KEY": "anthropic_api_key_sparkshost",
    "GEOAPIFY_KEY": "gtoapify_key_sfehost",
}

def keychain(item):
    try:
        out = subprocess.check_output(
            ["security", "find-generic-password", "-s", item, "-w"],
            stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except Exception:
        return None

out_path = os.path.join(env["PROJECT_INCLUDE_DIR"], "secrets.h")
lines = ["#pragma once", "// GENERATED — do not edit, do not commit."]
missing = []
for macro, item in ITEMS.items():
    val = keychain(item)
    if val is None:
        missing.append(item); val = ""
    lines.append(f'#define {macro} "{val}"')

if missing:
    sys.stderr.write(
        "gen_secrets: keychain items not found: " + ", ".join(missing) +
        "\n  Add them, or build without DEV_SECRETS to type keys on-device.\n")
    # Non-fatal: allow non-DEV builds. DEV_SECRETS builds will read empty keys.
with open(out_path, "w") as f:
    f.write("\n".join(lines) + "\n")
print("gen_secrets: wrote", out_path)
```

- [ ] **Step 3: Add the DEV_SECRETS flag to the device env**

Modify `platformio.ini` `[env:esp32s3]` `build_flags`, append `-DDEV_SECRETS`.

- [ ] **Step 4: Verify secrets generate**

Run: `pio run -e esp32s3 -t clean && pio run -e esp32s3` (compile only; okay if it fails later for missing app code — confirm `gen_secrets: wrote .../include/secrets.h` appears and `include/secrets.h` exists and is gitignored via `git status`).
Expected: `secrets.h` present, NOT staged by git.

- [ ] **Step 5: Commit**

```bash
git add scripts/gen_secrets.py include/secrets.example.h platformio.ini
git commit -m "feat: generate secrets.h from macOS keychain (DEV_SECRETS)"
```

---

## Phase 1 — Pure-logic core (native TDD)

All Phase 1 modules are platform-independent (no Arduino headers) so they build and test under `native`. Each is a `.h`/`.cpp` pair in `src/core/` with tests in `test/native/`.

### Task 3: Config model + defaults

**Files:**
- Create: `src/core/config_model.h`, `src/core/config_model.cpp`
- Create: `test/native/test_config/test_config.cpp`

**Interfaces:**
- Produces: `struct AppConfig { std::string anthropicKey, geoapifyKey, model, persona; int maxTokens; int toolLoopCap; bool sounds; std::string theme; int brightness; int sleepSeconds; };` and `AppConfig configDefaults();`.

- [ ] **Step 1: Write the failing test**

```cpp
#include <unity.h>
#include "core/config_model.h"
void test_defaults() {
  AppConfig c = configDefaults();
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", c.model.c_str());
  TEST_ASSERT_EQUAL(512, c.maxTokens);
  TEST_ASSERT_EQUAL(4, c.toolLoopCap);
  TEST_ASSERT_TRUE(c.sounds);
  TEST_ASSERT_EQUAL_STRING("roostos", c.theme.c_str());
}
int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_defaults); return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL** — `pio test -e native -f test_config` → fails to compile (no header).

- [ ] **Step 3: Implement `config_model.h/.cpp`**

```cpp
// config_model.h
#pragma once
#include <string>
struct AppConfig {
  std::string anthropicKey, geoapifyKey, model, persona, theme;
  int maxTokens; int toolLoopCap; bool sounds; int brightness; int sleepSeconds;
};
AppConfig configDefaults();
```
```cpp
// config_model.cpp
#include "core/config_model.h"
AppConfig configDefaults() {
  AppConfig c;
  c.model = "claude-haiku-4-5"; c.persona = "";
  c.maxTokens = 512; c.toolLoopCap = 4; c.sounds = true;
  c.theme = "roostos"; c.brightness = 80; c.sleepSeconds = 120;
  return c;
}
```

- [ ] **Step 4: Run, verify PASS** — `pio test -e native -f test_config`.
- [ ] **Step 5: Commit** — `git add -A && git commit -m "feat(core): AppConfig model + defaults"`.

### Task 4: Config serialization (key/value)

**Files:**
- Modify: `src/core/config_model.h/.cpp`
- Create: `test/native/test_config_serde/test_config_serde.cpp`

**Interfaces:**
- Produces: `std::map<std::string,std::string> configToKV(const AppConfig&);` and `AppConfig configFromKV(const std::map<std::string,std::string>&);` — a plain KV form the NVS wrapper (Task 15's `settings.cpp`) reads/writes one key at a time. Round-trips must preserve every field.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include "core/config_model.h"
void test_roundtrip() {
  AppConfig a = configDefaults();
  a.anthropicKey = "sk-ant-x"; a.geoapifyKey = "geo-y";
  a.persona = "Be terse."; a.maxTokens = 700; a.sounds = false;
  a.theme = "terminal"; a.brightness = 42; a.sleepSeconds = 30; a.toolLoopCap = 6;
  AppConfig b = configFromKV(configToKV(a));
  TEST_ASSERT_EQUAL_STRING(a.anthropicKey.c_str(), b.anthropicKey.c_str());
  TEST_ASSERT_EQUAL_STRING(a.geoapifyKey.c_str(), b.geoapifyKey.c_str());
  TEST_ASSERT_EQUAL_STRING(a.persona.c_str(), b.persona.c_str());
  TEST_ASSERT_EQUAL(a.maxTokens, b.maxTokens);
  TEST_ASSERT_EQUAL(a.toolLoopCap, b.toolLoopCap);
  TEST_ASSERT_FALSE(b.sounds);
  TEST_ASSERT_EQUAL_STRING("terminal", b.theme.c_str());
  TEST_ASSERT_EQUAL(42, b.brightness);
  TEST_ASSERT_EQUAL(30, b.sleepSeconds);
}
void test_missing_keys_fall_back_to_defaults() {
  AppConfig b = configFromKV({});
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", b.model.c_str());
  TEST_ASSERT_EQUAL(512, b.maxTokens);
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_roundtrip); RUN_TEST(test_missing_keys_fall_back_to_defaults);
  return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement `configToKV` / `configFromKV`** in `config_model.cpp` (header: add the two signatures and `#include <map>`).

```cpp
#include <map>
static int toInt(const std::map<std::string,std::string>& m, const char* k, int d){
  auto it=m.find(k); return it==m.end()?d:std::stoi(it->second);
}
static std::string toStr(const std::map<std::string,std::string>& m, const char* k, const std::string& d){
  auto it=m.find(k); return it==m.end()?d:it->second;
}
std::map<std::string,std::string> configToKV(const AppConfig& c){
  return {
    {"anthropicKey",c.anthropicKey},{"geoapifyKey",c.geoapifyKey},
    {"model",c.model},{"persona",c.persona},{"theme",c.theme},
    {"maxTokens",std::to_string(c.maxTokens)},{"toolLoopCap",std::to_string(c.toolLoopCap)},
    {"sounds",c.sounds?"1":"0"},{"brightness",std::to_string(c.brightness)},
    {"sleepSeconds",std::to_string(c.sleepSeconds)},
  };
}
AppConfig configFromKV(const std::map<std::string,std::string>& m){
  AppConfig c = configDefaults();
  c.anthropicKey=toStr(m,"anthropicKey",c.anthropicKey);
  c.geoapifyKey=toStr(m,"geoapifyKey",c.geoapifyKey);
  c.model=toStr(m,"model",c.model); c.persona=toStr(m,"persona",c.persona);
  c.theme=toStr(m,"theme",c.theme);
  c.maxTokens=toInt(m,"maxTokens",c.maxTokens); c.toolLoopCap=toInt(m,"toolLoopCap",c.toolLoopCap);
  c.sounds=toStr(m,"sounds","1")=="1"; c.brightness=toInt(m,"brightness",c.brightness);
  c.sleepSeconds=toInt(m,"sleepSeconds",c.sleepSeconds);
  return c;
}
```

- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): config KV serialization"`.

### Task 5: Chat history + token-budget trimming

**Files:**
- Create: `src/core/chat_history.h/.cpp`
- Create: `test/native/test_history/test_history.cpp`

**Interfaces:**
- Produces: `struct Turn { std::string role; std::string content; };` and `class ChatHistory { void addUser(std::string); void addAssistantRaw(std::string jsonContent); const std::vector<Turn>& turns() const; void clear(); void trimToApproxTokens(int budget); };` where an approx token = `chars/4`. Trimming removes oldest turns (never leaving the list empty if any turn exists) until under budget.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include "core/chat_history.h"
void test_add_and_clear() {
  ChatHistory h; h.addUser("hi"); TEST_ASSERT_EQUAL(1, h.turns().size());
  h.clear(); TEST_ASSERT_EQUAL(0, h.turns().size());
}
void test_trim_drops_oldest() {
  ChatHistory h;
  for (int i=0;i<10;i++) h.addUser(std::string(400,'x')); // ~100 tok each
  h.trimToApproxTokens(250); // keep ~2 newest
  TEST_ASSERT_LESS_OR_EQUAL(3, (int)h.turns().size());
  TEST_ASSERT_GREATER_OR_EQUAL(1, (int)h.turns().size());
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_add_and_clear); RUN_TEST(test_trim_drops_oldest);
  return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement `chat_history.h/.cpp`** (vector of `Turn`; `trimToApproxTokens` loops erasing `front()` while `>1` turn and sum(chars)/4 > budget).
- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): chat history + token trim"`.

### Task 6: Tool schema definitions

**Files:**
- Create: `src/core/tools_schema.h/.cpp`
- Create: `test/native/test_tools_schema/test_tools_schema.cpp`

**Interfaces:**
- Produces: `std::string toolsJson();` returning the Anthropic `tools` array (as a JSON string) for `get_location`, `show_map`, `get_battery`, `play_tone`, built with ArduinoJson. Consumed by Task 7's request builder.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include <ArduinoJson.h>
#include "core/tools_schema.h"
void test_tools_present() {
  JsonDocument d; deserializeJson(d, toolsJson());
  TEST_ASSERT_TRUE(d.is<JsonArray>());
  TEST_ASSERT_EQUAL(4, d.as<JsonArray>().size());
  // first tool has name + input_schema
  TEST_ASSERT_TRUE(d[0]["name"].is<const char*>());
  TEST_ASSERT_TRUE(d[0]["input_schema"]["type"].is<const char*>());
}
int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_tools_present); return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement `toolsJson()`** — build a `JsonDocument` array with the four tools (each `{name, description, input_schema:{type:"object", properties:{...}, required:[...]}}` per spec §7.1); `serializeJson` to a `std::string`. `show_map` props: `lat`,`lon`,`zoom` (all optional → `required:[]`); `play_tone` props `freq_hz`,`ms` required; `get_location`/`get_battery` empty objects.
- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): Anthropic tools schema"`.

### Task 7: Messages request builder

**Files:**
- Create: `src/core/claude_request.h/.cpp`
- Create: `test/native/test_request/test_request.cpp`

**Interfaces:**
- Consumes: `AppConfig` (Task 3), `ChatHistory` (Task 5), `toolsJson()` (Task 6).
- Produces: `std::string buildRequestBody(const AppConfig&, const ChatHistory&);` producing the Messages API JSON body: `{model, max_tokens, system?, tools, messages:[...]}` where each history `Turn` maps to `{role, content}` (content is a raw string for user turns; assistant raw-JSON content is embedded as parsed JSON).

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include <ArduinoJson.h>
#include "core/claude_request.h"
void test_body_shape() {
  AppConfig c = configDefaults(); c.persona = "Be brief.";
  ChatHistory h; h.addUser("Hello");
  JsonDocument d; deserializeJson(d, buildRequestBody(c, h));
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", d["model"]);
  TEST_ASSERT_EQUAL(512, d["max_tokens"].as<int>());
  TEST_ASSERT_EQUAL_STRING("Be brief.", d["system"]);
  TEST_ASSERT_EQUAL(4, d["tools"].as<JsonArray>().size());
  TEST_ASSERT_EQUAL_STRING("user", d["messages"][0]["role"]);
  TEST_ASSERT_EQUAL_STRING("Hello", d["messages"][0]["content"]);
}
void test_no_system_when_empty() {
  AppConfig c = configDefaults(); ChatHistory h; h.addUser("hi");
  JsonDocument d; deserializeJson(d, buildRequestBody(c, h));
  TEST_ASSERT_FALSE(d["system"].is<const char*>());
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_body_shape); RUN_TEST(test_no_system_when_empty);
  return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement `buildRequestBody`** — assemble with ArduinoJson; only set `system` when persona non-empty; parse `toolsJson()` into `tools`; for each turn, if `role=="assistant"` and content parses as JSON array, embed as array, else set string content.
- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): Messages request builder"`.

### Task 8: Response parser + tool-loop step

**Files:**
- Create: `src/core/claude_response.h/.cpp`
- Create: `test/native/test_response/test_response.cpp`

**Interfaces:**
- Produces:
  - `struct ToolCall { std::string id, name, inputJson; };`
  - `struct ParsedResponse { std::string stopReason; std::string assistantText; std::string assistantContentRaw; std::vector<ToolCall> toolCalls; int inputTokens, outputTokens; std::string error; };`
  - `ParsedResponse parseResponse(const std::string& json);`
  - `std::string buildToolResultTurn(const std::vector<std::pair<std::string,std::string>>& idToResult);` → a user turn `content` JSON array of `{type:"tool_result", tool_use_id, content}` blocks.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include "core/claude_response.h"
const char* TEXT_RESP = R"({"stop_reason":"end_turn","content":[{"type":"text","text":"Hi there"}],"usage":{"input_tokens":10,"output_tokens":3}})";
const char* TOOL_RESP = R"({"stop_reason":"tool_use","content":[{"type":"text","text":"Let me check"},{"type":"tool_use","id":"tu_1","name":"get_location","input":{}}],"usage":{"input_tokens":20,"output_tokens":8}})";
void test_text() {
  ParsedResponse r = parseResponse(TEXT_RESP);
  TEST_ASSERT_EQUAL_STRING("end_turn", r.stopReason.c_str());
  TEST_ASSERT_EQUAL_STRING("Hi there", r.assistantText.c_str());
  TEST_ASSERT_EQUAL(0, (int)r.toolCalls.size());
  TEST_ASSERT_EQUAL(3, r.outputTokens);
}
void test_tool() {
  ParsedResponse r = parseResponse(TOOL_RESP);
  TEST_ASSERT_EQUAL_STRING("tool_use", r.stopReason.c_str());
  TEST_ASSERT_EQUAL(1, (int)r.toolCalls.size());
  TEST_ASSERT_EQUAL_STRING("get_location", r.toolCalls[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("tu_1", r.toolCalls[0].id.c_str());
}
void test_error_body() {
  ParsedResponse r = parseResponse(R"({"type":"error","error":{"type":"authentication_error","message":"bad key"}})");
  TEST_ASSERT_EQUAL_STRING("authentication_error", r.error.c_str());
}
void test_tool_result_turn() {
  std::string t = buildToolResultTurn({{"tu_1","{\"lat\":38.6}"}});
  TEST_ASSERT_TRUE(t.find("tool_result")!=std::string::npos);
  TEST_ASSERT_TRUE(t.find("tu_1")!=std::string::npos);
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_text); RUN_TEST(test_tool); RUN_TEST(test_error_body); RUN_TEST(test_tool_result_turn);
  return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement** — parse with ArduinoJson: capture `stop_reason`, walk `content[]` concatenating `text` blocks into `assistantText` and collecting `tool_use` blocks into `toolCalls` (serialize each `input` back to `inputJson`); keep the whole `content` array serialized in `assistantContentRaw`; read `usage`; if top-level `type=="error"`, set `error` to `error.type`. `buildToolResultTurn` builds a JSON array of tool_result blocks and serializes it.
- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): response parser + tool_result assembly"`.

### Task 9: Usage accumulator + cost estimate

**Files:**
- Create: `src/core/usage_stats.h/.cpp`
- Create: `test/native/test_usage/test_usage.cpp`

**Interfaces:**
- Produces: `struct UsageStats { long inTokens=0, outTokens=0, lastIn=0, lastOut=0; };` `void accumulate(UsageStats&, int in, int out);` `double estimateCostUSD(const UsageStats&, const std::string& model);` — Haiku 4.5 rates $1/$5 per 1M in/out (from claude-api reference); unknown model → 0.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include "core/usage_stats.h"
void test_accumulate_and_cost() {
  UsageStats u; accumulate(u,1000000,0); accumulate(u,0,1000000);
  TEST_ASSERT_EQUAL(1000000, u.inTokens);
  TEST_ASSERT_EQUAL(1000000, u.outTokens);
  double c = estimateCostUSD(u,"claude-haiku-4-5"); // $1 + $5
  TEST_ASSERT_TRUE(c > 5.9 && c < 6.1);
}
int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_accumulate_and_cost); return UNITY_END(); }
```

- [ ] **Step 2–4:** FAIL → implement (`lastIn/lastOut` set to args, totals summed; cost = in/1e6*inRate + out/1e6*outRate) → PASS.
- [ ] **Step 5: Commit** — `git commit -am "feat(core): usage stats + cost estimate"`.

### Task 10: Geoapify URL builder

**Files:**
- Create: `src/core/map_url.h/.cpp`
- Create: `test/native/test_map_url/test_map_url.cpp`

**Interfaces:**
- Produces: `std::string geoapifyStaticUrl(double lat, double lon, int zoom, int w, int h, const std::string& key);` → a Geoapify Static Maps v1 URL, JPEG format, with a marker at the center.

- [ ] **Step 1: Failing test**

```cpp
#include <unity.h>
#include "core/map_url.h"
void test_url() {
  std::string u = geoapifyStaticUrl(38.6270, -90.1994, 15, 240, 176, "KEY123");
  TEST_ASSERT_TRUE(u.rfind("https://maps.geoapify.com/v1/staticmap",0)==0);
  TEST_ASSERT_TRUE(u.find("width=240")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("height=176")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("zoom=15")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("format=jpeg")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("apiKey=KEY123")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("lonlat:-90.1994,38.627")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("marker=")!=std::string::npos);
}
int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_url); return UNITY_END(); }
```

- [ ] **Step 2: Run, verify FAIL.**
- [ ] **Step 3: Implement** — format `.../staticmap?style=osm-bright&width=W&height=H&center=lonlat:LON,LAT&zoom=Z&format=jpeg&marker=lonlat:LON,LAT;color:%2334e2c0;size:medium&apiKey=KEY` with 6-dp coordinates via `snprintf`.
- [ ] **Step 4: Run, verify PASS.**
- [ ] **Step 5: Commit** — `git commit -am "feat(core): Geoapify static-map URL builder"`.

---

## Phase 2 — Hardware bringup (on-device)

> Hardware tasks cannot run under `native`. Each lists an explicit **on-device acceptance** check via serial monitor / visible behavior. Keep Phase 1 tests green (`pio test -e native`) after every task.

### Task 11: Capture the LILYGO T-Deck pin map

**Files:**
- Create: `include/config.h`

**Interfaces:**
- Produces: all pin macros + endpoint/brand constants consumed by every hardware/UI task: `PIN_POWERON (10)`, display SPI pins + `TFT_*`, `KB_I2C_ADDR (0x55)`, I2C SDA/SCL, trackball GPIOs, GPS UART pins + baud, I2S pins, battery ADC pin, LoRa pins (declared, unused in M1).

- [ ] **Step 1: Fetch ground truth** — open LILYGO's official `T-Deck` repo (`utilities.h` / `pins`/`board` headers for T-Deck **Plus**) and read the exact pin assignments. Do not proceed on guesses.
- [ ] **Step 2: Write `include/config.h`** transcribing those pins into named macros, plus: `#define ANTHROPIC_URL "https://api.anthropic.com/v1/messages"`, `#define ANTHROPIC_VERSION "2023-06-01"`, and the RoostOS palette hex constants from the Global Constraints.
- [ ] **Step 3: Compile** — `pio run -e esp32s3` (with the stub `main.cpp`).
Expected: builds clean.
- [ ] **Step 4: Commit** — `git commit -am "feat(hw): T-Deck Plus pin map + constants (from LILYGO repo)"`.

### Task 12: Board power-enable + display + LVGL + theme

**Files:**
- Create: `src/board/board.h/.cpp`
- Create: `src/ui/theme.h/.cpp`
- Create: `src/lv_conf.h` (LVGL config) and reference it via `-DLV_CONF_INCLUDE_SIMPLE` build flag
- Modify: `src/main.cpp`

**Interfaces:**
- Produces: `void boardInit();` (power-enable GPIO10 HIGH, init SPI display + LVGL display/flush buffer in PSRAM), `void boardTick();` (call `lv_timer_handler()`), and `void applyTheme(const char* name);` (Task 25 extends the palette table; here provide `"roostos"` only).

- [ ] **Step 1:** Add `lv_conf.h` (enable PSRAM buffer, 16-bit color, montserrat font) and `-DLV_CONF_INCLUDE_SIMPLE` to `esp32s3` build_flags.
- [ ] **Step 2:** Implement `boardInit()`: `pinMode(PIN_POWERON, OUTPUT); digitalWrite(PIN_POWERON, HIGH); delay(...)`; init the ST7789 via SPI and register an LVGL display + flush callback drawing into a PSRAM-allocated buffer.
- [ ] **Step 3:** Implement `applyTheme("roostos")` setting an LVGL theme with the brand palette; create a full-screen label "RoostOS Communicator" in teal on `#0d1117`.
- [ ] **Step 4:** `main.cpp`: `setup(){ boardInit(); applyTheme("roostos"); }` `loop(){ boardTick(); delay(5);}`.
- [ ] **On-device acceptance:** `pio run -e esp32s3 -t upload` → screen shows the teal title on dark background; serial prints "boardInit ok".
- [ ] **Step 5: Commit** — `git commit -am "feat(hw): power-enable, ST7789+LVGL, RoostOS theme"`.

### Task 13: Keyboard + trackball as LVGL input

**Files:**
- Create: `src/board/input.h/.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Produces: `void inputInit();` registering an LVGL keypad indev that reads the I2C keyboard (0x55) and maps trackball GPIO edges to LVGL `LV_KEY_UP/DOWN/LEFT/RIGHT/ENTER`; exposes `lv_indev_t* inputGroupIndev();`.

- [ ] **Step 1:** Implement `inputInit()`: begin I2C on config pins; LVGL keypad `read_cb` returns the keycode byte (0 = none), mapping trackball direction GPIOs and click to navigation keys.
- [ ] **Step 2:** Create an `lv_group_t`, attach the indev, and add a temporary text area so typing is visible.
- [ ] **On-device acceptance:** typing on the QWERTY appears in the text area; trackball moves focus.
- [ ] **Step 3: Commit** — `git commit -am "feat(hw): keyboard + trackball LVGL input"`.

### Task 14: GPS driver

**Files:**
- Create: `src/drivers/gps.h/.cpp`

**Interfaces:**
- Produces: `struct GpsFix { bool valid; double lat, lon; int sats; };` `void gpsInit();` `void gpsPump();` (call each loop to consume UART), `GpsFix gpsRead();`.

- [ ] **Step 1:** Implement NMEA parsing over the GPS UART (config pins/baud) — minimal `$GPGGA`/`$GPRMC` extraction of lat/lon/sats/fix. (A tiny hand parser or TinyGPS++ via lib_deps; if adding a lib, append to `esp32s3` lib_deps only.)
- [ ] **On-device acceptance:** outdoors or with antenna, serial prints a valid fix within a couple minutes; `gpsRead().valid` becomes true.
- [ ] **Step 2: Commit** — `git commit -am "feat(hw): u-blox GPS driver"`.

### Task 15: Battery driver

**Files:**
- Create: `src/drivers/battery.h/.cpp`

**Interfaces:**
- Produces: `struct Battery { float volts; int percent; bool charging; };` `Battery batteryRead();`.

- [ ] **Step 1:** Implement ADC read on the battery pin with the board's divider ratio → volts; map volts→percent (3.3V=0%, 4.2V=100%, clamped).
- [ ] **On-device acceptance:** serial prints a plausible voltage (~3.7–4.2V) and percent.
- [ ] **Step 2: Commit** — `git commit -am "feat(hw): battery driver"`.

### Task 16: Audio tone driver + sound gate

**Files:**
- Create: `src/drivers/audio.h/.cpp`

**Interfaces:**
- Produces: `void audioInit();` `void setSoundsEnabled(bool);` `void playTone(int freqHz, int ms);` — a no-op when sounds disabled.

- [ ] **Step 1:** Implement I2S sine/square tone generation for `ms` at `freqHz`; guard on the enabled flag.
- [ ] **On-device acceptance:** `playTone(880,200)` is audible when enabled, silent when disabled.
- [ ] **Step 2: Commit** — `git commit -am "feat(hw): I2S tone driver + sound gate"`.

---

## Phase 3 — Networking & tools

### Task 17: NVS settings store

**Files:**
- Create: `src/config/settings.h/.cpp`

**Interfaces:**
- Consumes: `configToKV`/`configFromKV` (Task 4).
- Produces: `AppConfig settingsLoad();` (reads each KV key from `Preferences`, applies `configFromKV`, seeds from `secrets.h` under `DEV_SECRETS` when the stored key is empty), `void settingsSave(const AppConfig&);`, plus saved-WiFi and saved-MAC list helpers: `std::vector<std::pair<String,String>> savedNetworks();` `void saveNetwork(String ssid,String pass);` `std::vector<String> savedMacs(); void saveMac(String);`.

- [ ] **Step 1:** Implement over `Preferences` (namespace `"roostcomm"`). On load, if `anthropicKey`/`geoapifyKey` empty and `DEV_SECRETS` defined, seed from `ANTHROPIC_API_KEY`/`GEOAPIFY_KEY` and persist.
- [ ] **On-device acceptance:** serial prints loaded model/theme; after `settingsSave` with a changed persona, a reboot shows the new persona. With `DEV_SECRETS`, keys are non-empty on first boot.
- [ ] **Step 2: Commit** — `git commit -am "feat(cfg): NVS settings store + keychain seed"`.

### Task 18: WiFi manager

**Files:**
- Create: `src/net/wifi_manager.h/.cpp`

**Interfaces:**
- Produces: `std::vector<WifiScanItem> wifiScan();` (`struct WifiScanItem{String ssid;int rssi;bool locked;}`), `bool wifiConnect(String ssid,String pass,uint32_t timeoutMs);`, `void wifiAutoConnect();` (try saved nets), `bool wifiIsUp();`, `String wifiIp(); String wifiMac(); String wifiSsid(); int wifiRssi();`.

- [ ] **Step 1:** Implement using `WiFi` STA: scan, connect with timeout + status, auto-connect looping saved networks (Task 17). On successful manual connect, persist via `saveNetwork`.
- [ ] **On-device acceptance:** device scans, connects to a known network, serial prints IP; reboot auto-reconnects.
- [ ] **Step 2: Commit** — `git commit -am "feat(net): WiFi manager + saved networks"`.

### Task 19: Captive-portal detection + MAC clone

**Files:**
- Create: `src/net/captive_portal.h/.cpp`

**Interfaces:**
- Produces: `bool captiveDetected();` (GET `http://connectivitycheck.gstatic.com/generate_204`; true if status ≠ 204 or redirected), `bool applyClonedMac(const String& mac);` (parse `AA:BB:..` → 6 bytes, `esp_wifi_set_mac(WIFI_IF_STA, bytes)` **before** connect), `String currentMac();`.

- [ ] **Step 1:** Implement detection via `HTTPClient` on port 80 with redirect following disabled; MAC parse + `esp_wifi_set_mac`. Expose a helper the WiFi screen calls before `wifiConnect` when a saved MAC is chosen.
- [ ] **On-device acceptance:** on an open network with a captive portal, `captiveDetected()` returns true and a banner is shown (banner UI in Task 22); after `applyClonedMac` with a phone-authenticated MAC and reconnect, internet check passes.
- [ ] **Step 2: Commit** — `git commit -am "feat(net): captive detection + MAC clone"`.

### Task 20: Claude HTTP client + tool loop

**Files:**
- Create: `src/net/claude_client.h/.cpp`

**Interfaces:**
- Consumes: `buildRequestBody` (T7), `parseResponse`/`buildToolResultTurn` (T8), `UsageStats` (T9), `ChatHistory` (T5), `device_tools` dispatch (T21).
- Produces: `struct ClaudeResult { std::string text; std::string error; };` `ClaudeResult claudeSend(const AppConfig&, ChatHistory&, UsageStats&, std::function<std::string(const std::string& name,const std::string& inputJson)> toolDispatch);` — runs the bounded loop: POST body (WiFiClientSecure, `setInsecure()` acceptable for M1, or bundled root CA), parse, if `tool_use` dispatch each tool, append assistant turn + tool_result turn, repeat up to `cfg.toolLoopCap`; accumulate usage each response; return final text or a readable error.

- [ ] **Step 1:** Implement the transport + loop; map HTTP 401→"bad API key", 429→"rate limited", timeouts→"no response"; on cap reached return a "stopped after N tool steps" note.
- [ ] **On-device acceptance:** with WiFi up and a valid key, sending "hi" returns Claude text on serial; sending "where am I?" triggers `get_location` (see Task 21) and returns a location-aware reply.
- [ ] **Step 2: Commit** — `git commit -am "feat(net): Claude client + agentic tool loop"`.

### Task 21: Map client + device tools dispatch

**Files:**
- Create: `src/net/map_client.h/.cpp`
- Create: `src/tools/device_tools.h/.cpp`

**Interfaces:**
- Consumes: `geoapifyStaticUrl` (T10), `gpsRead` (T14), `batteryRead` (T15), `playTone` (T16), settings for the Geoapify key.
- Produces:
  - `map_client`: `bool fetchMapToCanvas(double lat,double lon,int zoom,lv_obj_t* imgParent, String& err);` — build URL, HTTP GET JPEG into a PSRAM buffer, decode with TJpg_Decoder into an LVGL image object appended under `imgParent`.
  - `device_tools`: `std::string dispatchTool(const std::string& name,const std::string& inputJson);` returning the `tool_result` text; `show_map` calls `fetchMapToCanvas` against the chat's image container (set via `void setMapTarget(lv_obj_t*)`), defaulting lat/lon to `gpsRead()` when absent.

- [ ] **Step 1:** Implement `dispatchTool` switching on name: `get_location`→JSON of fix; `get_battery`→JSON of battery; `play_tone`→parse `freq_hz`/`ms`, call `playTone`, return "played"; `show_map`→parse optional coords/zoom (default GPS + zoom 15), call `fetchMapToCanvas`, return "map displayed at lat,lon" or an error string.
- [ ] **Step 2:** Implement `fetchMapToCanvas` (HTTPS GET, buffer JPEG, `TJpgDec` decode callback writes pixels into an `lv_img` buffer). On any failure set `err` and return false.
- [ ] **On-device acceptance:** "show me a map of where I am" renders a map image bubble centered on the GPS fix; battery/tone tools work end to end.
- [ ] **Step 3: Commit** — `git commit -am "feat: device tools dispatch + Geoapify map render"`.

---

## Phase 4 — UI screens

### Task 22: Screen manager + boot + menu

**Files:**
- Create: `src/ui/ui.h/.cpp`
- Create: `src/ui/screen_boot.cpp`, `src/ui/screen_menu.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Produces: `void uiInit();` (build screens, show boot then menu), `void uiShow(const char* screen);` navigation among `"boot"|"menu"|"chat"|"wifi"|"stats"|"settings"`. Menu lists Chat · WiFi · Stats · Settings and routes trackball/Enter to `uiShow`.

- [ ] **Step 1:** Implement boot splash → after `boardInit`/`settingsLoad`/`wifiAutoConnect`, transition to menu (a themed list bound to the input group).
- [ ] **Step 2:** `main.cpp` `setup()` calls `boardInit → applyTheme(cfg.theme) → inputInit → settingsLoad → wifiAutoConnect → uiInit`.
- [ ] **On-device acceptance:** boot splash appears, then a navigable menu; selecting an item switches screens.
- [ ] **Step 3: Commit** — `git commit -am "feat(ui): screen manager, boot, menu"`.

### Task 23: WiFi screen (scan/connect/captive/clone)

**Files:**
- Create: `src/ui/screen_wifi.cpp`

**Interfaces:**
- Consumes: `wifiScan/wifiConnect` (T18), `captiveDetected/applyClonedMac` (T19), saved MACs (T17).

- [ ] **Step 1:** Build a scrollable network list (SSID + RSSI + lock icon); on select, prompt for password (on-screen keyboard entry), connect with a spinner + result. After connect, run `captiveDetected`; if true, show the amber **captive banner** with a **Clone MAC** action (pick/enter a saved MAC → `applyClonedMac` → reconnect). Rescan + Forget actions.
- [ ] **On-device acceptance:** can join a WPA network by typing a password; a captive network shows the banner and the clone flow.
- [ ] **Step 2: Commit** — `git commit -am "feat(ui): WiFi screen + captive/clone"`.

### Task 24: Chat screen (bubbles, input, images, clear)

**Files:**
- Create: `src/ui/screen_chat.cpp`

**Interfaces:**
- Consumes: `claudeSend` (T20), `dispatchTool`/`setMapTarget` (T21), `ChatHistory` (T5), `UsageStats` (T9).

- [ ] **Step 1:** Build a scrolling message area (user bubbles right/indigo, assistant left/panel) + a bottom input line. On send: append a user bubble, call `claudeSend(cfg, history, usage, dispatchTool)` (blocking with a "…" typing indicator), append the assistant bubble. Register the message area as the `show_map` image target via `setMapTarget`.
- [ ] **Step 2:** Add a **Clear** button: confirm dialog → `history.clear()`, remove bubbles/images, reset session `UsageStats`.
- [ ] **On-device acceptance:** a full conversation works; "show a map of my location" renders an inline map bubble; Clear empties the chat and resets tokens.
- [ ] **Step 3: Commit** — `git commit -am "feat(ui): chat screen with tools, maps, clear"`.

### Task 25: Settings screen

**Files:**
- Create: `src/ui/screen_settings.cpp`

**Interfaces:**
- Consumes: `settingsLoad/settingsSave` (T17), `applyTheme` (T26), `setSoundsEnabled` (T16).

- [ ] **Step 1:** Build editable fields: Anthropic key, Geoapify key, model, max_tokens, persona, tool-loop cap, saved MACs; toggles/selectors: **Sounds on/off** (→ `setSoundsEnabled`), **Theme** (→ `applyTheme` live), **Brightness** (→ backlight PWM), **Sleep timeout**. Save persists via `settingsSave`.
- [ ] **On-device acceptance:** changing persona/model/max_tokens affects the next chat; toggling sounds mutes tones; changing theme restyles live; changes survive reboot.
- [ ] **Step 2: Commit** — `git commit -am "feat(ui): settings screen"`.

### Task 26: Stats screen

**Files:**
- Create: `src/ui/screen_stats.cpp`

**Interfaces:**
- Consumes: `UsageStats`+`estimateCostUSD` (T9), `wifi*` (T18), `gpsRead` (T14), `batteryRead` (T15), `version.h`.

- [ ] **Step 1:** Create `include/version.h` with `#define ROOST_COMM_VERSION "0.1.0-m1"`.
- [ ] **Step 2:** Build a read-only, periodically-refreshed panel: tokens (session in/out + last + est. cost), network (SSID/RSSI/IP/MAC/captive), device (GPS fix+sats, battery V/%, uptime, free heap, free PSRAM, version).
- [ ] **On-device acceptance:** after a chat, tokens/cost are non-zero and match usage; device rows show live values.
- [ ] **Step 3: Commit** — `git commit -am "feat(ui): stats screen"`.

### Task 27: Theme table + live switching

**Files:**
- Modify: `src/ui/theme.h/.cpp`

**Interfaces:**
- Produces: a palette table for `"roostos"|"light"|"terminal"|"spiderverse"` consumed by `applyTheme` (T12), restyling all open screens.

- [ ] **Step 1:** Add a `struct Palette { uint32_t bg,panel,edge,ink,dim,accent,accent2,warn; };` table with the four entries (RoostOS from constants; Light = inverted brand; Terminal = amber-on-black; Spider-Verse = magenta/cyan on near-black). `applyTheme(name)` looks up + applies, defaulting to `roostos` on unknown.
- [ ] **On-device acceptance:** switching theme in Settings immediately restyles menu/chat/stats.
- [ ] **Step 2: Commit** — `git commit -am "feat(ui): theme table + live switching"`.

---

## Phase 5 — Integration & acceptance

### Task 28: End-to-end acceptance + docs

**Files:**
- Modify: `README.md`
- Create: `PROTOCOL.md` placeholder note (LoRa protocol deferred to M3)

- [ ] **Step 1: Full native suite** — `pio test -e native` all green.
- [ ] **Step 2: On-device acceptance script** (manual checklist, record results in README):
  1. Fresh flash with `DEV_SECRETS` → boots, keys seeded, auto-joins WiFi.
  2. Captive network → banner + MAC-clone → internet.
  3. Chat round-trip with Haiku.
  4. "Where am I, and show me a map" → `get_location` + `show_map` render a map bubble.
  5. `get_battery` and `play_tone` via natural language.
  6. Clear chat resets history + tokens.
  7. Stats shows tokens/cost + device details.
  8. Theme switch + sounds toggle persist across reboot.
- [ ] **Step 3:** Update README with build/flash/first-boot/keychain instructions and the matching-key caveat.
- [ ] **Step 4: Commit + push** — `git commit -am "docs: M1 acceptance + README" && git push`.

---

## Self-Review

**Spec coverage:** §1 goal → all phases; §2 hardware → T11–T16; §3 toolchain → T1; §4 modules → mapped 1:1 across tasks; §5 theme → T12/T27; §6 boot flow → T22; §7 chat+tool loop → T5–T8, T20; §7.1 tools → T6, T21; §8 map → T10, T21; §9 WiFi/captive → T18/T19; §10 chat features/clear → T24; §10a sounds → T16/T25; §10b stats → T9/T26; §10c themes → T27; §11 provisioning → T2/T17; §11a niceties → T25; §12 errors → T20/T21; §13 testing → Phase 1 + T28. No uncovered requirement found.

**Placeholder scan:** No "TBD"/"add error handling"/"similar to Task N". Hardware tasks intentionally specify on-device acceptance instead of native code because they cannot run under `native`; pin values are sourced from LILYGO in T11 rather than invented.

**Type consistency:** `AppConfig`, `ChatHistory`, `ParsedResponse`/`ToolCall`, `UsageStats`, `dispatchTool`, `geoapifyStaticUrl`, `applyTheme`, `setSoundsEnabled`, `setMapTarget` names are used identically across producing and consuming tasks.
