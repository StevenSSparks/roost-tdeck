#include "core/claude_request.h"
#include <ArduinoJson.h>
#include "core/tools_schema.h"

std::string buildRequestBody(const AppConfig& config, const ChatHistory& history) {
  JsonDocument doc;

  // Set model and max_tokens
  doc["model"] = config.model;
  doc["max_tokens"] = config.maxTokens;

  // Set system only if persona is non-empty
  if (!config.persona.empty()) {
    doc["system"] = config.persona;
  }

  // Parse toolsJson() into tools array
  JsonDocument toolsDoc;
  deserializeJson(toolsDoc, toolsJson());
  doc["tools"] = toolsDoc.as<JsonArray>();

  // Add messages from ChatHistory
  JsonArray messagesArray = doc["messages"].to<JsonArray>();

  for (const auto& turn : history.turns()) {
    JsonObject msg = messagesArray.add<JsonObject>();
    msg["role"] = turn.role;

    // For assistant messages, try to parse content as JSON array
    if (turn.role == "assistant") {
      JsonDocument contentDoc;
      DeserializationError error = deserializeJson(contentDoc, turn.content);

      // If it's a valid JSON array, embed as array; otherwise set as string
      if (!error && contentDoc.is<JsonArray>()) {
        msg["content"] = contentDoc.as<JsonArray>();
      } else {
        msg["content"] = turn.content;
      }
    } else {
      // For user messages, always set as string
      msg["content"] = turn.content;
    }
  }

  // Serialize to string and return
  std::string result;
  serializeJson(doc, result);
  return result;
}
