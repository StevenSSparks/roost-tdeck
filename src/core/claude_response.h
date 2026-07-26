#ifndef CLAUDE_RESPONSE_H
#define CLAUDE_RESPONSE_H

#include <string>
#include <vector>

struct ToolCall {
  std::string id;
  std::string name;
  std::string inputJson;
};

struct ParsedResponse {
  std::string stopReason;
  std::string assistantText;
  std::string assistantContentRaw;
  std::vector<ToolCall> toolCalls;
  int inputTokens;
  int outputTokens;
  std::string error;
};

ParsedResponse parseResponse(const std::string& json);
std::string buildToolResultTurn(const std::vector<std::pair<std::string,std::string>>& idToResult);

#endif // CLAUDE_RESPONSE_H
