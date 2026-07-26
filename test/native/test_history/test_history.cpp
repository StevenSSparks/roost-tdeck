#include <unity.h>
#include "core/chat_history.h"
void test_add_and_clear() {
  ChatHistory h; h.addUser("hi"); TEST_ASSERT_EQUAL(1, h.turns().size());
  h.clear(); TEST_ASSERT_EQUAL(0, h.turns().size());
}
void test_trim_drops_oldest() {
  ChatHistory h;
  for (int i=0;i<10;i++) h.addUser(std::string(400,'x')); // ~100 tok each
  h.trimToApproxTokens(250); // keep ~2 newest
  TEST_ASSERT_LESS_OR_EQUAL(3, (int)h.turns().size());
  TEST_ASSERT_GREATER_OR_EQUAL(1, (int)h.turns().size());
}
int main(int,char**){ UNITY_BEGIN();
  RUN_TEST(test_add_and_clear); RUN_TEST(test_trim_drops_oldest);
  return UNITY_END(); }
