#include "core/claude_response.h"
#include <ArduinoJson.h>

ParsedResponse parseResponse(const std::string& json) {
  ParsedResponse result;
  result.inputTokens = 0;
  result.outputTokens = 0;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    result.error = "deserialization_error";
    return result;
  }

  // Check if this is an error response
  if (doc["type"] == "error") {
    result.error = doc["error"]["type"].as<std::string>();
    return result;
  }

  // Extract stop_reason
  if (doc["stop_reason"].is<const char*>()) {
    result.stopReason = doc["stop_reason"].as<std::string>();
  }

  // Process content array
  if (doc["content"].is<JsonArray>()) {
    JsonArray contentArray = doc["content"].as<JsonArray>();

    // Serialize the entire content array for assistantContentRaw
    serializeJson(contentArray, result.assistantContentRaw);

    // Walk through content blocks
    for (JsonVariant item : contentArray) {
      std::string type = item["type"].as<std::string>();

      if (type == "text") {
        // Concatenate text blocks
        result.assistantText += item["text"].as<std::string>();
      } else if (type == "tool_use") {
        // Collect tool_use blocks
        ToolCall tc;
        tc.id = item["id"].as<std::string>();
        tc.name = item["name"].as<std::string>();

        // Serialize the input object back to JSON string
        serializeJson(item["input"], tc.inputJson);

        result.toolCalls.push_back(tc);
      }
    }
  }

  // Extract usage tokens
  if (doc["usage"].is<JsonObject>()) {
    result.inputTokens = doc["usage"]["input_tokens"].as<int>();
    result.outputTokens = doc["usage"]["output_tokens"].as<int>();
  }

  return result;
}

std::string buildToolResultTurn(const std::vector<std::pair<std::string,std::string>>& idToResult) {
  JsonDocument doc;
  JsonArray contentArray = doc.to<JsonArray>();

  for (const auto& pair : idToResult) {
    JsonObject toolResultBlock = contentArray.add<JsonObject>();
    toolResultBlock["type"] = "tool_result";
    toolResultBlock["tool_use_id"] = pair.first;
    toolResultBlock["content"] = pair.second;
  }

  std::string result;
  serializeJson(contentArray, result);
  return result;
}
