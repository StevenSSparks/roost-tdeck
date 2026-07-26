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
