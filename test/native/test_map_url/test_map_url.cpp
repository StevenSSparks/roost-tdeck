#include <unity.h>
#include "core/map_url.h"

void test_url() {
  std::string u = geoapifyStaticUrl(38.6270, -90.1994, 15, 240, 176, "KEY123");
  TEST_ASSERT_TRUE(u.rfind("https://maps.geoapify.com/v1/staticmap",0)==0);
  TEST_ASSERT_TRUE(u.find("width=240")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("height=176")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("zoom=15")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("format=jpeg")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("apiKey=KEY123")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("lonlat:-90.1994,38.627")!=std::string::npos);
  TEST_ASSERT_TRUE(u.find("marker=")!=std::string::npos);
}

int main(int,char**){ UNITY_BEGIN(); RUN_TEST(test_url); return UNITY_END(); }
