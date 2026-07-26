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
