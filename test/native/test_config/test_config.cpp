#include <unity.h>
#include "core/config_model.h"
void test_defaults() {
  AppConfig c = configDefaults();
  TEST_ASSERT_EQUAL_STRING("claude-haiku-4-5", c.model.c_str());
  TEST_ASSERT_EQUAL(512, c.maxTokens);
  TEST_ASSERT_EQUAL(4, c.toolLoopCap);
  TEST_ASSERT_TRUE(c.sounds);
  TEST_ASSERT_EQUAL_STRING("roostos", c.theme.c_str());
  TEST_ASSERT_EQUAL(8, c.kbBacklight);
}
int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_defaults); return UNITY_END(); }
