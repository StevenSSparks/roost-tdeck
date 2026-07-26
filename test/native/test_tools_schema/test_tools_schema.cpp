#include <unity.h>
#include <ArduinoJson.h>
#include "core/tools_schema.h"

void test_tools_present() {
  JsonDocument d;
  deserializeJson(d, toolsJson());
  TEST_ASSERT_TRUE(d.is<JsonArray>());
  TEST_ASSERT_EQUAL(4, d.as<JsonArray>().size());
  // first tool has name + input_schema
  TEST_ASSERT_TRUE(d[0]["name"].is<const char*>());
  TEST_ASSERT_TRUE(d[0]["input_schema"]["type"].is<const char*>());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tools_present);
  return UNITY_END();
}
