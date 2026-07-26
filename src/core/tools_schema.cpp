#include "tools_schema.h"
#include <ArduinoJson.h>

std::string toolsJson() {
  JsonDocument doc;
  JsonArray tools = doc.to<JsonArray>();

  // Tool 1: get_location
  {
    JsonObject tool = tools.add<JsonObject>();
    tool["name"] = "get_location";
    tool["description"] = "Get the current device location";
    JsonObject schema = tool["input_schema"].to<JsonObject>();
    schema["type"] = "object";
    schema["properties"].to<JsonObject>();  // empty properties
    schema["required"].to<JsonArray>();      // empty required
  }

  // Tool 2: show_map
  {
    JsonObject tool = tools.add<JsonObject>();
    tool["name"] = "show_map";
    tool["description"] = "Display a map on the device screen";
    JsonObject schema = tool["input_schema"].to<JsonObject>();
    schema["type"] = "object";
    JsonObject props = schema["properties"].to<JsonObject>();
    props["lat"]["type"] = "number";
    props["lon"]["type"] = "number";
    props["zoom"]["type"] = "number";
    schema["required"].to<JsonArray>();      // empty required (all optional)
  }

  // Tool 3: get_battery
  {
    JsonObject tool = tools.add<JsonObject>();
    tool["name"] = "get_battery";
    tool["description"] = "Get the device battery status";
    JsonObject schema = tool["input_schema"].to<JsonObject>();
    schema["type"] = "object";
    schema["properties"].to<JsonObject>();  // empty properties
    schema["required"].to<JsonArray>();      // empty required
  }

  // Tool 4: play_tone
  {
    JsonObject tool = tools.add<JsonObject>();
    tool["name"] = "play_tone";
    tool["description"] = "Play a tone on the device speaker";
    JsonObject schema = tool["input_schema"].to<JsonObject>();
    schema["type"] = "object";
    JsonObject props = schema["properties"].to<JsonObject>();
    props["freq_hz"]["type"] = "number";
    props["ms"]["type"] = "number";
    JsonArray req = schema["required"].to<JsonArray>();
    req.add("freq_hz");
    req.add("ms");
  }

  std::string result;
  serializeJson(doc, result);
  return result;
}
