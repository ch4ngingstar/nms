// Golden-corpus cross-check for the firmware envelope layer
// (CLAUDE.md "the golden corpus decides").
//
// The same protocol/golden/ fixtures that pin the Python validator are replayed
// through the firmware's parseCmd so the two implementations agree byte-for-byte
// on what a `cmd` envelope is. Runs under `pio test -e native` (envelope.cpp
// pulls in ArduinoJson); the corpus is read from disk at run time.
//
// GOLDEN_DIR defaults to the repo layout (protocol/golden lives one level above
// the firmware/ project dir); override with -DGOLDEN_DIR=... if invoked elsewhere.

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "envelope.h"

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "../protocol/golden"
#endif

void setUp(void) {}
void tearDown(void) {}

// Reads a fixture into buf; returns its length, or -1 if it could not be opened
// (so a wrong working directory degrades to a skip, not a spurious failure).
static long readFixture(const char* rel, char* buf, size_t size) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", GOLDEN_DIR, rel);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return -1;
    }
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

// Valid cmd fixtures must parse and yield the advertised command name.
static void assertCmdFixture(const char* rel, const char* expectCmd,
                             const char* expectJobId) {
    static char raw[2048];
    long n = readFixture(rel, raw, sizeof(raw));
    if (n < 0) {
        TEST_IGNORE_MESSAGE("golden corpus not found at GOLDEN_DIR (set -DGOLDEN_DIR)");
        return;
    }
    char cmd[24], job[36], args[512];
    TEST_ASSERT_TRUE_MESSAGE(
        parseCmd(raw, (size_t)n, cmd, sizeof(cmd), job, sizeof(job),
                 args, sizeof(args)),
        rel);
    TEST_ASSERT_EQUAL_STRING(expectCmd, cmd);
    TEST_ASSERT_EQUAL_STRING(expectJobId, job);
}

// Fixtures that are not valid cmd envelopes must be rejected by parseCmd.
static void assertRejected(const char* rel) {
    static char raw[2048];
    long n = readFixture(rel, raw, sizeof(raw));
    if (n < 0) {
        TEST_IGNORE_MESSAGE("golden corpus not found at GOLDEN_DIR (set -DGOLDEN_DIR)");
        return;
    }
    char cmd[24], job[36], args[512];
    TEST_ASSERT_FALSE_MESSAGE(
        parseCmd(raw, (size_t)n, cmd, sizeof(cmd), job, sizeof(job),
                 args, sizeof(args)),
        rel);
}

static void test_valid_cmd_fixtures_parse(void) {
    assertCmdFixture("valid/cmd_port_scan.json", "port_scan", "job-7f3a91");
    assertCmdFixture("valid/cmd_set_monitor.json", "set_monitor", "job-7f3a96");
    assertCmdFixture("valid/cmd_wifi_survey.json", "wifi_survey", "job-7f3a95");
}

static void test_non_cmd_valid_fixtures_are_not_cmds(void) {
    // Well-formed envelopes of other types are correctly rejected by the cmd gate.
    assertRejected("valid/status_online.json");
    assertRejected("valid/result_accepted.json");
    assertRejected("valid/telemetry_basic.json");
}

static void test_invalid_fixtures_rejected(void) {
    assertRejected("invalid/wrong_protocol_version.json");
    assertRejected("invalid/unknown_message_type.json");
    assertRejected("invalid/ts_in_milliseconds.json");
    assertRejected("invalid/missing_envelope_ts.json");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_cmd_fixtures_parse);
    RUN_TEST(test_non_cmd_valid_fixtures_are_not_cmds);
    RUN_TEST(test_invalid_fixtures_rejected);
    return UNITY_END();
}
