#include <unity.h>
#include "TempNodeCore.h"

using namespace tempnode;

void test_compare_version_strings() {
  TEST_ASSERT_EQUAL(0, compareVersionStrings("1.2.3", "1.2.3"));
  TEST_ASSERT_TRUE(compareVersionStrings("1.2.10", "1.2.3") > 0);
  TEST_ASSERT_TRUE(compareVersionStrings("1.2.0", "1.2") == 0);
  TEST_ASSERT_TRUE(compareVersionStrings("v2.0.0", "1.9.9") > 0);
  TEST_ASSERT_TRUE(compareVersionStrings("build-2026", "build-2025") > 0);
}

void test_parse_hex_digest() {
  const char* good = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  uint8_t out[32] = {0};
  TEST_ASSERT_TRUE(parseHexDigest(good, out, sizeof(out)));
  TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
  TEST_ASSERT_EQUAL_HEX8(0xEF, out[15]);
  TEST_ASSERT_EQUAL_HEX8(0xEF, out[31]);

  TEST_ASSERT_FALSE(parseHexDigest("abc", out, sizeof(out)));
  TEST_ASSERT_FALSE(parseHexDigest("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", out, sizeof(out)));
}

void test_parse_history_timestamp_ms() {
  uint64_t ts = 0;
  TEST_ASSERT_TRUE(parseHistoryTimestampMs("{\"timestamp\":1741862400123,\"sensorId\":\"28FF\"}", ts));
  TEST_ASSERT_EQUAL_UINT64(1741862400123ULL, ts);

  TEST_ASSERT_TRUE(parseHistoryTimestampMs("{\"x\":1, \"timestamp\": 42, \"y\":2}", ts));
  TEST_ASSERT_EQUAL_UINT64(42ULL, ts);

  TEST_ASSERT_FALSE(parseHistoryTimestampMs("{\"sensorId\":\"28FF\"}", ts));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_compare_version_strings);
  RUN_TEST(test_parse_hex_digest);
  RUN_TEST(test_parse_history_timestamp_ms);
  return UNITY_END();
}
