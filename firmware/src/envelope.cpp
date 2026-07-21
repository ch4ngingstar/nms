#include "envelope.h"

#include <string.h>
#include <ArduinoJson.h>

// Local NUL-terminating bounded copy. Avoids depending on strlcpy, which is
// present on the ESP32 but not guaranteed by the host toolchain the native
// unit tests build under.
static void copyStr(char* dst, const char* src, size_t dstSize) {
    if (dstSize == 0) {
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dstSize; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Randomness source for msg_id. On the ESP32 the hardware RNG is used; on the
// host build (native unit tests) rand() is enough — msg_id uniqueness is not a
// security property here, only a collision-avoidance one.
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include <esp_random.h>
static uint32_t randWord() { return esp_random(); }
#else
#include <stdlib.h>
static uint32_t randWord() {
    return ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 3) ^ (uint32_t)rand();
}
#endif

size_t generateMsgId(char* buf, size_t bufSize) {
    if (bufSize < MSG_ID_HEX_LEN + 1) {
        return 0;
    }
    // 16 hex chars = 8 random bytes = two 32-bit words.
    uint32_t hi = randWord();
    uint32_t lo = randWord();
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        buf[i] = HEX[(hi >> (28 - i * 4)) & 0xF];
        buf[8 + i] = HEX[(lo >> (28 - i * 4)) & 0xF];
    }
    buf[MSG_ID_HEX_LEN] = '\0';
    return MSG_ID_HEX_LEN;
}

size_t buildEnvelope(char* out, size_t outSize, const char* type,
                     const char* node, uint32_t ts, const char* dataJson) {
    char msgId[MSG_ID_HEX_LEN + 1];
    if (generateMsgId(msgId, sizeof(msgId)) == 0) {
        return 0;
    }

    JsonDocument doc;
    doc["v"] = 1;
    doc["type"] = type;  // const char* — stored by reference, outlives the call
    doc["node"] = node;
    doc["msg_id"] = msgId;  // char[] — ArduinoJson copies it into its own pool
    doc["ts"] = ts;
    // serialized() embeds the pre-built data object verbatim rather than
    // reparsing it — keeps `data` opaque to this layer.
    doc["data"] = serialized(dataJson);

    // Reject before writing if the frame would blow the buffer or the protocol
    // payload cap. measureJson excludes the trailing NUL.
    size_t needed = measureJson(doc);
    if (needed + 1 > outSize || needed > PAYLOAD_MAX_BYTES) {
        return 0;
    }
    return serializeJson(doc, out, outSize);
}

bool parseCmd(const char* json, size_t len,
              char* cmdOut, size_t cmdSize,
              char* jobIdOut, size_t jobIdSize,
              char* argsOut, size_t argsSize) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
        return false;
    }

    // Envelope gate: wrong version or type is a rejection, not
    // a best-effort parse.
    if (doc["v"] != 1 || strcmp(doc["type"] | "", "cmd") != 0) {
        return false;
    }

    JsonObjectConst data = doc["data"];
    const char* cmd = data["cmd"] | (const char*)nullptr;
    const char* jobId = data["job_id"] | (const char*)nullptr;
    if (cmd == nullptr || jobId == nullptr) {
        return false;
    }
    if (cmdSize == 0 || jobIdSize == 0 || argsSize < 3) {
        return false;
    }

    copyStr(cmdOut, cmd, cmdSize);
    copyStr(jobIdOut, jobId, jobIdSize);

    // args is optional; serialize the object when present, else emit an empty
    // object so downstream always parses valid JSON.
    JsonVariantConst args = data["args"];
    if (args.is<JsonObjectConst>()) {
        size_t n = serializeJson(args, argsOut, argsSize);
        if (n == 0 || n + 1 > argsSize) {
            return false;  // args present but did not fit — reject rather than truncate
        }
    } else {
        copyStr(argsOut, "{}", argsSize);
    }
    return true;
}
