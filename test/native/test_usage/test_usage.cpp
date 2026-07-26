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
