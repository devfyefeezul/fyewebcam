// Checks the C++ wire format against the same byte vectors the Android unit tests use.
#include <cstdio>
#include <cstring>

#include "../src/protocol.h"

static int failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                            \
        }                                                          \
    } while (0)

int main() {
    uint8_t hello[proto::kHandshakeSize];
    proto::WriteHello({1280, 720, 30, proto::kCameraFront}, hello);
    const uint8_t expected_hello[] = {'P', 'W', 'C', 'M', 1, 0x00, 0x05, 0xD0, 0x02, 30, 1};
    CHECK(std::memcmp(hello, expected_hello, sizeof(hello)) == 0);

    const uint8_t reply_bytes[] = {0x50, 0x57, 0x43, 0x4D, 0, 0x80, 0x07, 0x38, 0x04, 30, 1};
    proto::Reply reply{};
    CHECK(proto::ParseReply(reply_bytes, &reply));
    CHECK(reply.status == proto::kStatusOk);
    CHECK(reply.width == 1920 && reply.height == 1080 && reply.fps == 30);
    CHECK(reply.codec == proto::kCodecH264);

    uint8_t bad[proto::kHandshakeSize] = {};
    CHECK(!proto::ParseReply(bad, &reply));

    const uint8_t header_bytes[] = {2, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0};
    proto::FrameHeader header = proto::ParseFrameHeader(header_bytes);
    CHECK(header.type == proto::kFrameKey);
    CHECK(header.pts_us == 0x0102);
    CHECK(header.length == 5);

    if (failures == 0) std::printf("protocol_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
