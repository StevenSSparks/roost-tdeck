#include <unity.h>
void test_sanity() { TEST_ASSERT_EQUAL(4, 2 + 2); }
int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sanity);
  return UNITY_END();
}
