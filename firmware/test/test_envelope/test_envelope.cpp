// Native unit tests for envelope build/parse (protocol §5.2, §7.1).
//
// Uses ArduinoJson (header-only, host-portable) so it runs under `pio test -e
// native`, not bare g++. Pins msg_id shape, the envelope field layout, the
// 1024-byte payload cap, and the cmd-envelope gate parseCmd enforces.

#include <unity.h>
#include <string.h>

#include "envelope.h"

void setUp(void) {}
void tearDown(void) {}

static bool isLowerHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// --- generateMsgId --------------------------------------------------------

static void test_msgid_is_16_lower_hex(void) {
    char id[MSG_ID_HEX_LEN + 1];
    TEST_ASSERT_EQUAL_UINT(MSG_ID_HEX_LEN, generateMsgId(id, sizeof(id)));
    TEST_ASSERT_EQUAL_UINT(16, strlen(id));
    for (size_t i = 0; i < MSG_ID_HEX_LEN; ++i) {
        TEST_ASSERT_TRUE(isLowerHex(id[i]));
    }
}

static void test_msgid_rejects_small_buffer(void) {
    char id[MSG_ID_HEX_LEN];  // one short of the required 17
    TEST_ASSERT_EQUAL_UINT(0, generateMsgId(id, sizeof(id)));
}

// --- buildEnvelope --------------------------------------------------------

static void test_build_envelope_layout(void) {
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), "status", "probe-a4c1f8",
                             1755302400u, "{\"state\":\"online\"}");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT(n, strlen(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"v\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"type\":\"status\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"node\":\"probe-a4c1f8\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ts\":1755302400"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"msg_id\":\""));
    // The data object is embedded verbatim, not re-parsed.
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"data\":{\"state\":\"online\"}"));
}

static void test_build_envelope_rejects_oversize_payload(void) {
    // A data object whose string value alone blows past the 1024-byte cap.
    static char data[PAYLOAD_MAX_BYTES + 64];
    size_t p = 0;
    const char* head = "{\"k\":\"";
    for (const char* h = head; *h; ++h) data[p++] = *h;
    for (int i = 0; i < 1100; ++i) data[p++] = 'x';
    data[p++] = '"';
    data[p++] = '}';
    data[p] = '\0';

    static char out[2048];
    TEST_ASSERT_EQUAL_UINT(0, buildEnvelope(out, sizeof(out), "result",
                                            "probe-a4c1f8", 1755302400u, data));
}

static void test_build_envelope_rejects_small_output(void) {
    char tiny[16];
    TEST_ASSERT_EQUAL_UINT(0, buildEnvelope(tiny, sizeof(tiny), "status",
                                            "probe-a4c1f8", 1755302400u,
                                            "{\"state\":\"online\"}"));
}

// --- parseCmd -------------------------------------------------------------

static void test_parse_cmd_extracts_fields(void) {
    const char* json =
        "{\"v\":1,\"type\":\"cmd\",\"node\":\"probe-a4c1f8\","
        "\"msg_id\":\"abcdef0123456789\",\"ts\":1755302400,"
        "\"data\":{\"cmd\":\"port_scan\",\"job_id\":\"job-7f3a91\","
        "\"args\":{\"ports\":\"22,80\"}}}";
    char cmd[24], job[36], args[256];
    TEST_ASSERT_TRUE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                              job, sizeof(job), args, sizeof(args)));
    TEST_ASSERT_EQUAL_STRING("port_scan", cmd);
    TEST_ASSERT_EQUAL_STRING("job-7f3a91", job);
    TEST_ASSERT_NOT_NULL(strstr(args, "\"ports\":\"22,80\""));
}

static void test_parse_cmd_defaults_missing_args_to_empty_object(void) {
    const char* json =
        "{\"v\":1,\"type\":\"cmd\",\"node\":\"probe-a4c1f8\","
        "\"msg_id\":\"abcdef0123456789\",\"ts\":1755302400,"
        "\"data\":{\"cmd\":\"identify\",\"job_id\":\"job-2\"}}";
    char cmd[24], job[36], args[16];
    TEST_ASSERT_TRUE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                              job, sizeof(job), args, sizeof(args)));
    TEST_ASSERT_EQUAL_STRING("{}", args);
}

static void test_parse_cmd_rejects_wrong_version(void) {
    const char* json =
        "{\"v\":2,\"type\":\"cmd\",\"data\":{\"cmd\":\"identify\",\"job_id\":\"j\"}}";
    char cmd[24], job[36], args[16];
    TEST_ASSERT_FALSE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                               job, sizeof(job), args, sizeof(args)));
}

static void test_parse_cmd_rejects_wrong_type(void) {
    const char* json =
        "{\"v\":1,\"type\":\"status\",\"data\":{\"cmd\":\"identify\",\"job_id\":\"j\"}}";
    char cmd[24], job[36], args[16];
    TEST_ASSERT_FALSE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                               job, sizeof(job), args, sizeof(args)));
}

static void test_parse_cmd_rejects_missing_job_id(void) {
    const char* json =
        "{\"v\":1,\"type\":\"cmd\",\"data\":{\"cmd\":\"identify\"}}";
    char cmd[24], job[36], args[16];
    TEST_ASSERT_FALSE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                               job, sizeof(job), args, sizeof(args)));
}

static void test_parse_cmd_rejects_malformed_json(void) {
    const char* json = "{not valid json";
    char cmd[24], job[36], args[16];
    TEST_ASSERT_FALSE(parseCmd(json, strlen(json), cmd, sizeof(cmd),
                               job, sizeof(job), args, sizeof(args)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_msgid_is_16_lower_hex);
    RUN_TEST(test_msgid_rejects_small_buffer);
    RUN_TEST(test_build_envelope_layout);
    RUN_TEST(test_build_envelope_rejects_oversize_payload);
    RUN_TEST(test_build_envelope_rejects_small_output);
    RUN_TEST(test_parse_cmd_extracts_fields);
    RUN_TEST(test_parse_cmd_defaults_missing_args_to_empty_object);
    RUN_TEST(test_parse_cmd_rejects_wrong_version);
    RUN_TEST(test_parse_cmd_rejects_wrong_type);
    RUN_TEST(test_parse_cmd_rejects_missing_job_id);
    RUN_TEST(test_parse_cmd_rejects_malformed_json);
    return UNITY_END();
}
