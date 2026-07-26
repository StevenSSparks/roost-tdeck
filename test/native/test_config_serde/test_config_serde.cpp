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
  TEST_ASSERT_EQUAL(a.kbBacklight, b.kbBacklight);
}
void test_missing_keys_fall_back_to_defaults() {
  AppConfig b = configFromKV({});
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", b.model.c_str());
  TEST_ASSERT_EQUAL(512, b.maxTokens);
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_roundtrip); RUN_TEST(test_missing_keys_fall_back_to_defaults);
  return UNITY_END(); }
