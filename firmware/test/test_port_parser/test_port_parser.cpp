// Native unit tests for parsePortSpec (spec §7.2, §9.1) — the C++ side of the
// protocol/ports.py contract. The parser is pure; port_scan.cpp's socket paths
// are #ifdef'd out on native, so only this parser links here.
//
// Mirrors parse_ports(): ascending, de-duplicated, ranges expanded, and the same
// rejections (empty element, non-decimal, out of 1..65535, inverted range).

#include <unity.h>

#include "port_scan.h"

void setUp(void) {}
void tearDown(void) {}

static void test_simple_list(void) {
    uint16_t out[16];
    int n = parsePortSpec("22,80,443", out, 16);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT16(22, out[0]);
    TEST_ASSERT_EQUAL_UINT16(80, out[1]);
    TEST_ASSERT_EQUAL_UINT16(443, out[2]);
}

static void test_range_expands_inclusive(void) {
    uint16_t out[16];
    int n = parsePortSpec("8000-8003", out, 16);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_UINT16(8000, out[0]);
    TEST_ASSERT_EQUAL_UINT16(8001, out[1]);
    TEST_ASSERT_EQUAL_UINT16(8002, out[2]);
    TEST_ASSERT_EQUAL_UINT16(8003, out[3]);
}

static void test_sorted_and_deduplicated(void) {
    uint16_t out[16];
    int n = parsePortSpec("443,22,80,22,443", out, 16);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT16(22, out[0]);
    TEST_ASSERT_EQUAL_UINT16(80, out[1]);
    TEST_ASSERT_EQUAL_UINT16(443, out[2]);
}

static void test_mixed_list_and_range(void) {
    uint16_t out[32];
    int n = parsePortSpec("80,20-22,443", out, 32);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_UINT16(20, out[0]);
    TEST_ASSERT_EQUAL_UINT16(21, out[1]);
    TEST_ASSERT_EQUAL_UINT16(22, out[2]);
    TEST_ASSERT_EQUAL_UINT16(80, out[3]);
    TEST_ASSERT_EQUAL_UINT16(443, out[4]);
}

static void test_whitespace_is_trimmed(void) {
    uint16_t out[16];
    int n = parsePortSpec("  22 , 80 ", out, 16);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT16(22, out[0]);
    TEST_ASSERT_EQUAL_UINT16(80, out[1]);
}

static void test_inverted_range_rejected(void) {
    uint16_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("100-50", out, 16));
}

static void test_empty_element_rejected(void) {
    uint16_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("22,,80", out, 16));
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("", out, 16));
}

static void test_out_of_range_rejected(void) {
    uint16_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("70000", out, 16));  // > 65535
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("0", out, 16));      // < 1
}

static void test_non_decimal_rejected(void) {
    uint16_t out[16];
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("22a", out, 16));
    TEST_ASSERT_EQUAL_INT(-1, parsePortSpec("0x50", out, 16));
}

// A range wider than the buffer fills what fits and returns that count (spec §7).
static void test_maxports_clamp_fills_what_fits(void) {
    uint16_t out[4];
    int n = parsePortSpec("1-100", out, 4);
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_UINT16(1, out[0]);
    TEST_ASSERT_EQUAL_UINT16(4, out[3]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_list);
    RUN_TEST(test_range_expands_inclusive);
    RUN_TEST(test_sorted_and_deduplicated);
    RUN_TEST(test_mixed_list_and_range);
    RUN_TEST(test_whitespace_is_trimmed);
    RUN_TEST(test_inverted_range_rejected);
    RUN_TEST(test_empty_element_rejected);
    RUN_TEST(test_out_of_range_rejected);
    RUN_TEST(test_non_decimal_rejected);
    RUN_TEST(test_maxports_clamp_fills_what_fits);
    return UNITY_END();
}
