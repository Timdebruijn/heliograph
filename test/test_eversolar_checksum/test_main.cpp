// SPDX-License-Identifier: MIT
// Checksum and frame construction.

#include <unity.h>

#include <cstring>

#include "protocols/pmu/pmu_protocol.h"
#include "fixtures/eversolar_frames.h"

using namespace heliograph::pmu;
namespace fx = heliograph::fixtures;

void setUp() {}
void tearDown() {}

// --- checksum ---------------------------------------------------------------------------

static void test_checksum_is_plain_byte_sum() {
    // AA + 55 + 01 + 00 + 00 + 10 + 11 + 02 + 00 = 0x123
    const uint8_t body[] = {0xAA, 0x55, 0x01, 0x00, 0x00, 0x10, 0x11, 0x02, 0x00};
    TEST_ASSERT_EQUAL_UINT16(0x0123, checksum(body, sizeof(body)));
}

static void test_checksum_includes_the_header() {
    const uint8_t withHeader[] = {0xAA, 0x55, 0x01};
    const uint8_t withoutHeader[] = {0x01};
    // Guards against the classic mistake of starting the sum after the AA 55.
    TEST_ASSERT_NOT_EQUAL(checksum(withoutHeader, sizeof(withoutHeader)),
                          checksum(withHeader, sizeof(withHeader)));
}

static void test_checksum_wraps_at_16_bits() {
    uint8_t big[1000];
    std::memset(big, 0xFF, sizeof(big));  // 1000 * 255 = 255000 -> wraps
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(255000u & 0xFFFF),
                             checksum(big, sizeof(big)));
}

static void test_checksum_of_empty_is_zero() {
    TEST_ASSERT_EQUAL_UINT16(0, checksum(nullptr, 0));
}

// --- buildRequest -----------------------------------------------------------------------

static void test_build_request_matches_fixture() {
    uint8_t out[kMaxFrameSize];
    size_t  len = 0;
    const auto r = buildRequest(cmd::kQueryNormalInfo, inverterAddress(0x10), nullptr, 0, out,
                                sizeof(out), len);
    TEST_ASSERT_EQUAL(BuildResult::Ok, r);
    TEST_ASSERT_EQUAL_size_t(fx::kReqQueryNormalInfoLen, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fx::kReqQueryNormalInfo, out, len);
}

static void test_build_request_broadcast_matches_fixture() {
    uint8_t out[kMaxFrameSize];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::Ok, buildRequest(cmd::kOfflineQuery, kBroadcastAddress,
                                                    nullptr, 0, out, sizeof(out), len));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fx::kReqOfflineQuery, out, len);
}

static void test_build_request_with_payload_matches_fixture() {
    // SEND_REGISTER_ADDRESS carries the serial number plus the address we hand out.
    uint8_t payload[32];
    const size_t serialLen = std::strlen(fx::kExpectedSerial);
    std::memcpy(payload, fx::kExpectedSerial, serialLen);
    payload[serialLen] = kFirstInverterAddress;

    uint8_t out[kMaxFrameSize];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::Ok,
                      buildRequest(cmd::kSendAddress, kBroadcastAddress, payload, serialLen + 1,
                                   out, sizeof(out), len));
    TEST_ASSERT_EQUAL_size_t(fx::kReqSendAddressLen, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fx::kReqSendAddress, out, len);
}

static void test_build_request_sets_pmu_as_source() {
    uint8_t out[kMaxFrameSize];
    size_t  len = 0;
    buildRequest(cmd::kQueryNormalInfo, inverterAddress(0x10), nullptr, 0, out, sizeof(out), len);
    TEST_ASSERT_EQUAL_UINT8(kPmuAddress.high, out[kOffsetSourceHigh]);
    TEST_ASSERT_EQUAL_UINT8(kPmuAddress.low, out[kOffsetSourceHigh + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[kOffsetDestinationHigh]);
    TEST_ASSERT_EQUAL_UINT8(0x10, out[kOffsetDestinationHigh + 1]);
}

static void test_build_request_rejects_oversized_payload() {
    uint8_t payload[kMaxDataLength + 1] = {};
    uint8_t out[kMaxFrameSize + 16];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::DataTooLong,
                      buildRequest(cmd::kSendAddress, kBroadcastAddress, payload, sizeof(payload),
                                   out, sizeof(out), len));
}

static void test_build_request_rejects_small_buffer() {
    uint8_t out[kFrameOverhead - 1];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::BufferTooSmall,
                      buildRequest(cmd::kQueryNormalInfo, inverterAddress(0x10), nullptr, 0, out,
                                   sizeof(out), len));
}

// --- round trip -------------------------------------------------------------------------

static void test_built_request_parses_back() {
    uint8_t out[kMaxFrameSize];
    size_t  len = 0;
    buildRequest(cmd::kQueryNormalInfo, inverterAddress(0x10), nullptr, 0, out, sizeof(out), len);

    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok, parseFrame(out, len, f));
    TEST_ASSERT_EQUAL_UINT8(0x11, f.control);
    TEST_ASSERT_EQUAL_UINT8(0x02, f.function);
    TEST_ASSERT_EQUAL_size_t(0, f.dataLength);
    TEST_ASSERT_EQUAL_size_t(len, f.frameLength);
}

// --- checksum validation on receive ------------------------------------------------------

static void test_corrupt_checksum_is_rejected() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::BadChecksum,
                      parseFrame(fx::kRespBadChecksum, fx::kRespBadChecksumLen, f));
}

static void test_a_bad_checksum_still_reports_the_frame_length_for_resync() {
    // The driver skips `frameLength` bytes to resync after a corrupt frame. Header and
    // length byte were already validated on this path, so the length is trustworthy;
    // leaving it at 0 degraded resync to one byte per attempt through the whole frame.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::BadChecksum,
                      parseFrame(fx::kRespBadChecksum, fx::kRespBadChecksumLen, f));
    TEST_ASSERT_EQUAL_size_t(fx::kRespBadChecksumLen, f.frameLength);
}

static void test_all_zero_frame_is_rejected() {
    // An idle bus reading as zeros must never surface as a real zero measurement. Its
    // checksum "passes" (0 == 0), which is why the reference implementation needs an
    // explicit csum > 0 guard -- it does not validate the header. We do, so the frame is
    // rejected earlier and on a stronger ground.
    TEST_ASSERT_EQUAL_UINT16(0, checksum(fx::kAllZeroFrame, fx::kAllZeroFrameLen - 2));

    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::BadHeader,
                      parseFrame(fx::kAllZeroFrame, fx::kAllZeroFrameLen, f));
}

static void test_checksum_of_zero_is_valid_when_the_sum_wraps() {
    // Justifies dropping the reference's csum > 0 guard: with a real AA 55 header the byte
    // sum can still land on exactly 65536 and truncate to 0. That is a correct checksum,
    // and the guard would have rejected the frame.
    uint8_t frame[kFrameOverhead + 255];
    frame[0] = kHeader0;  // 170
    frame[1] = kHeader1;  //  85  -> 255
    frame[2] = 0x01;      //   1  -> 256 (PMU source)
    frame[3] = 0x00;
    frame[4] = 0x00;
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;
    frame[8] = 0xFF;      // 255  -> 511, payload length 255
    for (size_t i = 0; i < 255; ++i) {
        frame[kOffsetData + i] = 0xFF;  // 255 * 255 = 65025 -> 65536
    }
    const uint16_t sum = checksum(frame, kOffsetData + 255);
    TEST_ASSERT_EQUAL_UINT16(0, sum);

    frame[kOffsetData + 255]     = 0x00;
    frame[kOffsetData + 255 + 1] = 0x00;

    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok, parseFrame(frame, sizeof(frame), f));
}

static void test_valid_response_passes_checksum() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespNormalInfoSingle, fx::kRespNormalInfoSingleLen, f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_is_plain_byte_sum);
    RUN_TEST(test_checksum_includes_the_header);
    RUN_TEST(test_checksum_wraps_at_16_bits);
    RUN_TEST(test_checksum_of_empty_is_zero);
    RUN_TEST(test_build_request_matches_fixture);
    RUN_TEST(test_build_request_broadcast_matches_fixture);
    RUN_TEST(test_build_request_with_payload_matches_fixture);
    RUN_TEST(test_build_request_sets_pmu_as_source);
    RUN_TEST(test_build_request_rejects_oversized_payload);
    RUN_TEST(test_build_request_rejects_small_buffer);
    RUN_TEST(test_built_request_parses_back);
    RUN_TEST(test_corrupt_checksum_is_rejected);
    RUN_TEST(test_a_bad_checksum_still_reports_the_frame_length_for_resync);
    RUN_TEST(test_all_zero_frame_is_rejected);
    RUN_TEST(test_checksum_of_zero_is_valid_when_the_sum_wraps);
    RUN_TEST(test_valid_response_passes_checksum);
    return UNITY_END();
}
