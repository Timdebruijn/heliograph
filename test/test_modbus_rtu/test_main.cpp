// SPDX-License-Identifier: MIT
// Modbus RTU codec: CRC, request framing, response validation and register decoding.
// CRC constants are computed independently (Python, poly 0xA001) and cross-checked against the
// canonical Modbus vector 01 03 00 00 00 01 -> 84 0A, not taken from memory.

#include <unity.h>

#include <cstdint>
#include <cstring>

#include "protocols/modbus/modbus_rtu.h"

using namespace heliograph::modbus;

void setUp() {}
void tearDown() {}

// --- CRC ----------------------------------------------------------------------------------

static void test_crc_matches_the_canonical_vector() {
    const uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX16(0x0A84, crc16(frame, sizeof(frame)));
}

static void test_crc_second_vector() {
    const uint8_t frame[] = {0x11, 0x03, 0x00, 0x6B, 0x00, 0x03};
    TEST_ASSERT_EQUAL_HEX16(0x8776, crc16(frame, sizeof(frame)));
}

// --- request framing ----------------------------------------------------------------------

static void test_read_request_is_framed_low_crc_byte_first() {
    uint8_t out[16];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::Ok,
                      buildReadRequest(0x01, kReadHoldingRegisters, 0x0000, 1, out, sizeof(out), len));
    const uint8_t expected[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_write_single_register_is_framed() {
    uint8_t out[16];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::Ok,
                      buildWriteSingleRegister(0x01, 0x0001, 0x0003, out, sizeof(out), len));
    const uint8_t expected[] = {0x01, 0x06, 0x00, 0x01, 0x00, 0x03, 0x98, 0x0B};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_write_multiple_registers_carries_byte_count_and_big_endian_data() {
    uint8_t        out[32];
    size_t         len    = 0;
    const uint16_t vals[] = {0x000A, 0x0102};
    TEST_ASSERT_EQUAL(BuildResult::Ok,
                      buildWriteMultipleRegisters(0x01, 0x0010, vals, 2, out, sizeof(out), len));
    // unit fn addrHi addrLo qtyHi qtyLo byteCount data... crcLo crcHi
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x10, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[6]);  // 2 registers = 4 bytes
    TEST_ASSERT_EQUAL_HEX8(0x00, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0x0A, out[8]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[9]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[10]);
}

static void test_zero_quantity_is_rejected() {
    uint8_t out[16];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::InvalidQuantity,
                      buildReadRequest(0x01, kReadHoldingRegisters, 0, 0, out, sizeof(out), len));
}

static void test_over_max_quantity_is_rejected() {
    uint8_t out[16];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::InvalidQuantity,
                      buildReadRequest(0x01, kReadHoldingRegisters, 0, 126, out, sizeof(out), len));
}

static void test_too_small_buffer_is_rejected() {
    uint8_t out[4];
    size_t  len = 0;
    TEST_ASSERT_EQUAL(BuildResult::BufferTooSmall,
                      buildReadRequest(0x01, kReadHoldingRegisters, 0, 1, out, sizeof(out), len));
}

// --- response parsing ---------------------------------------------------------------------

// Build a valid read reply with a correct CRC, for the parser to chew on.
static size_t makeReadReply(uint8_t unit, uint8_t fn, const uint16_t* regs, uint8_t count,
                            uint8_t* out) {
    out[0] = unit;
    out[1] = fn;
    out[2] = static_cast<uint8_t>(count * 2);
    for (uint8_t i = 0; i < count; ++i) {
        out[3 + i * 2] = static_cast<uint8_t>((regs[i] >> 8) & 0xFF);
        out[4 + i * 2] = static_cast<uint8_t>(regs[i] & 0xFF);
    }
    const size_t   dataEnd = 3 + static_cast<size_t>(count) * 2;
    const uint16_t crc     = crc16(out, dataEnd);
    out[dataEnd]     = static_cast<uint8_t>(crc & 0xFF);
    out[dataEnd + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return dataEnd + 2;
}

static void test_read_response_decodes_registers_big_endian() {
    const uint16_t regs[] = {0x1234, 0x00FF, 0xABCD};
    uint8_t        frame[32];
    const size_t   len = makeReadReply(0x01, kReadHoldingRegisters, regs, 3, frame);

    uint16_t     decoded[8] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseReadResponse(frame, len, 0x01, kReadHoldingRegisters, decoded, 8, resp));
    TEST_ASSERT_EQUAL_UINT8(3, resp.registerCount);
    TEST_ASSERT_EQUAL_HEX16(0x1234, decoded[0]);
    TEST_ASSERT_EQUAL_HEX16(0x00FF, decoded[1]);
    TEST_ASSERT_EQUAL_HEX16(0xABCD, decoded[2]);
}

static void test_a_corrupt_crc_is_rejected() {
    const uint16_t regs[] = {0x1234};
    uint8_t        frame[16];
    size_t         len = makeReadReply(0x01, kReadHoldingRegisters, regs, 1, frame);
    frame[len - 1] ^= 0xFF;  // wreck the CRC

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::BadCrc,
                      parseReadResponse(frame, len, 0x01, kReadHoldingRegisters, decoded, 4, resp));
}

static void test_a_reply_from_another_unit_is_rejected() {
    const uint16_t regs[] = {0x1234};
    uint8_t        frame[16];
    const size_t   len = makeReadReply(0x02, kReadHoldingRegisters, regs, 1, frame);

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::WrongUnit,
                      parseReadResponse(frame, len, 0x01, kReadHoldingRegisters, decoded, 4, resp));
}

static void test_a_function_mismatch_is_rejected() {
    const uint16_t regs[] = {0x1234};
    uint8_t        frame[16];
    const size_t   len = makeReadReply(0x01, kReadInputRegisters, regs, 1, frame);

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::WrongFunction,
                      parseReadResponse(frame, len, 0x01, kReadHoldingRegisters, decoded, 4, resp));
}

static void test_an_exception_reply_is_reported_with_its_code() {
    // unit, fn|0x80, exceptionCode, CRC. 0x02 = illegal data address.
    uint8_t frame[8];
    frame[0]           = 0x01;
    frame[1]           = kReadHoldingRegisters | kExceptionFlag;
    frame[2]           = 0x02;
    const uint16_t crc = crc16(frame, 3);
    frame[3]           = static_cast<uint8_t>(crc & 0xFF);
    frame[4]           = static_cast<uint8_t>((crc >> 8) & 0xFF);

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::Exception,
                      parseReadResponse(frame, 5, 0x01, kReadHoldingRegisters, decoded, 4, resp));
    TEST_ASSERT_EQUAL_HEX8(0x02, resp.exceptionCode);
}

static void test_an_exception_for_a_different_function_is_rejected() {
    // Exception echoing function 0x04, but we asked 0x03: a stale/misrouted frame, not our
    // answer, even though unit and CRC are fine.
    uint8_t frame[8];
    frame[0]           = 0x01;
    frame[1]           = kReadInputRegisters | kExceptionFlag;  // 0x84, not our 0x03
    frame[2]           = 0x02;
    const uint16_t crc = crc16(frame, 3);
    frame[3]           = static_cast<uint8_t>(crc & 0xFF);
    frame[4]           = static_cast<uint8_t>((crc >> 8) & 0xFF);

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::WrongFunction,
                      parseReadResponse(frame, 5, 0x01, kReadHoldingRegisters, decoded, 4, resp));
}

static void test_a_partial_frame_asks_for_more() {
    const uint16_t regs[] = {0x1234, 0x5678};
    uint8_t        frame[16];
    const size_t   len = makeReadReply(0x01, kReadHoldingRegisters, regs, 2, frame);

    uint16_t     decoded[4] = {0};
    ReadResponse resp;
    // Hand the parser everything except the last two bytes.
    TEST_ASSERT_EQUAL(ParseResult::Incomplete,
                      parseReadResponse(frame, len - 2, 0x01, kReadHoldingRegisters, decoded, 4, resp));
}

static void test_registers_overflowing_the_caller_buffer_are_refused() {
    const uint16_t regs[] = {0x1111, 0x2222, 0x3333};
    uint8_t        frame[32];
    const size_t   len = makeReadReply(0x01, kReadHoldingRegisters, regs, 3, frame);

    uint16_t     decoded[2] = {0};  // too small for 3
    ReadResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::Malformed,
                      parseReadResponse(frame, len, 0x01, kReadHoldingRegisters, decoded, 2, resp));
}

static void test_write_single_echo_is_validated() {
    uint8_t out[16];
    size_t  len = 0;
    buildWriteSingleRegister(0x01, 0x0001, 0x0003, out, sizeof(out), len);
    // A compliant device echoes the request verbatim.
    WriteResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseWriteResponse(out, len, 0x01, kWriteSingleRegister, resp));
    TEST_ASSERT_EQUAL_HEX16(0x0001, resp.address);
    TEST_ASSERT_EQUAL_HEX16(0x0003, resp.value);
}

static void test_write_exception_is_reported() {
    uint8_t frame[8];
    frame[0]           = 0x01;
    frame[1]           = kWriteSingleRegister | kExceptionFlag;
    frame[2]           = 0x04;  // slave device failure
    const uint16_t crc = crc16(frame, 3);
    frame[3]           = static_cast<uint8_t>(crc & 0xFF);
    frame[4]           = static_cast<uint8_t>((crc >> 8) & 0xFF);

    WriteResponse resp;
    TEST_ASSERT_EQUAL(ParseResult::Exception,
                      parseWriteResponse(frame, 5, 0x01, kWriteSingleRegister, resp));
    TEST_ASSERT_EQUAL_HEX8(0x04, resp.exceptionCode);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_the_canonical_vector);
    RUN_TEST(test_crc_second_vector);
    RUN_TEST(test_read_request_is_framed_low_crc_byte_first);
    RUN_TEST(test_write_single_register_is_framed);
    RUN_TEST(test_write_multiple_registers_carries_byte_count_and_big_endian_data);
    RUN_TEST(test_zero_quantity_is_rejected);
    RUN_TEST(test_over_max_quantity_is_rejected);
    RUN_TEST(test_too_small_buffer_is_rejected);
    RUN_TEST(test_read_response_decodes_registers_big_endian);
    RUN_TEST(test_a_corrupt_crc_is_rejected);
    RUN_TEST(test_a_reply_from_another_unit_is_rejected);
    RUN_TEST(test_a_function_mismatch_is_rejected);
    RUN_TEST(test_an_exception_reply_is_reported_with_its_code);
    RUN_TEST(test_an_exception_for_a_different_function_is_rejected);
    RUN_TEST(test_a_partial_frame_asks_for_more);
    RUN_TEST(test_registers_overflowing_the_caller_buffer_are_refused);
    RUN_TEST(test_write_single_echo_is_validated);
    RUN_TEST(test_write_exception_is_reported);
    return UNITY_END();
}
