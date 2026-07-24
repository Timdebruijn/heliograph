// SPDX-License-Identifier: MIT
//
// The shared Modbus read transaction. This used to live inside the Growatt driver, where its
// only coverage was whatever the Growatt tests happened to exercise. It is now the exchange
// that BOTH the Growatt map and SunSpec run on, so the failure modes get pinned directly:
// a device that refuses, one that says nothing, one that answers corruption, and one that
// dribbles bytes forever without ever completing a frame.

#include <unity.h>

#include <vector>

#include "protocols/modbus/modbus_client.h"
#include "support/mock_transport.h"

using namespace heliograph;
using namespace heliograph::modbus;
using heliograph::test::MockTransport;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint8_t  kUnit = 1;
constexpr uint16_t kFrom = 40000;

/// Builds a well-formed read reply carrying `values`, CRC included.
std::vector<uint8_t> goodReply(uint8_t unit, uint8_t fn, const std::vector<uint16_t>& values) {
    std::vector<uint8_t> f{unit, fn, static_cast<uint8_t>(values.size() * 2)};
    for (const uint16_t v : values) {
        f.push_back(static_cast<uint8_t>(v >> 8));
        f.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    const uint16_t crc = crc16(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

std::vector<uint8_t> exceptionReply(uint8_t unit, uint8_t fn, uint8_t code) {
    std::vector<uint8_t> f{unit, static_cast<uint8_t>(fn | 0x80), code};
    const uint16_t       crc = crc16(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc & 0xFF));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    return f;
}

}  // namespace

static void test_a_good_reply_decodes() {
    MockTransport t;
    t.replyWith(goodReply(kUnit, kReadHoldingRegisters, {0x5375, 0x6e53, 0x0001}));

    uint16_t   regs[3] = {};
    const auto r =
        readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 3, regs, 3);

    TEST_ASSERT_EQUAL(ReadStatus::Ok, r.status);
    TEST_ASSERT_EQUAL_UINT16(0x5375, regs[0]);  // "Su"
    TEST_ASSERT_EQUAL_UINT16(0x6e53, regs[1]);  // "nS"
    TEST_ASSERT_EQUAL_UINT16(0x0001, regs[2]);
}

static void test_an_exception_reports_its_code() {
    MockTransport t;
    t.replyWith(exceptionReply(kUnit, kReadHoldingRegisters, 0x02));  // illegal data address

    uint16_t   regs[4] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 4, regs, 4);

    TEST_ASSERT_EQUAL(ReadStatus::Exception, r.status);
    TEST_ASSERT_EQUAL_UINT8(0x02, r.exceptionCode);
}

static void test_silence_is_a_timeout() {
    MockTransport t;
    t.replyWithSilence();

    uint16_t   regs[2] = {};
    const auto r = readRegisters(t, kUnit, kReadInputRegisters, 0, 2, regs, 2);

    TEST_ASSERT_EQUAL(ReadStatus::Timeout, r.status);
}

static void test_a_corrupt_frame_is_protocol_not_timeout() {
    MockTransport t;
    auto bad = goodReply(kUnit, kReadHoldingRegisters, {0x1234});
    bad.back() ^= 0xFF;  // wreck the CRC
    t.replyWith(bad);

    uint16_t   regs[1] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 1, regs, 1);

    // The distinction matters in the field: Timeout means nothing came back (wiring, a
    // sleeping device), Protocol means bytes came back corrupted (electrical noise).
    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
}

static void test_a_reply_from_another_unit_is_refused() {
    MockTransport t;
    t.replyWith(goodReply(kUnit + 1, kReadHoldingRegisters, {0x1111}));

    uint16_t   regs[1] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 1, regs, 1);

    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
}

// The reason this loop has a wall-clock deadline at all: each read() renews its own timeout,
// so a device trickling bytes that never form a frame would otherwise hold the bus forever.
static void test_an_endless_trickle_still_hits_the_deadline() {
    MockTransport t;
    t.infiniteNoise = true;
    // The noise has to keep the parser INCOMPLETE, which is the case the deadline exists for.
    // Random bytes do not: a leading 0x00 is simply the wrong unit id and the parser rejects
    // the frame at once (that path is covered above). So this trickles a plausible header for
    // our own unit and function announcing 200 payload bytes -- a 205-byte frame that never
    // finishes arriving, so the parser keeps politely asking for more until the clock runs out.
    t.noisePattern = {kUnit, kReadHoldingRegisters, 0xC8};
    t.msPerRead    = 50;  // the simulated clock only advances on read()

    uint16_t   regs[125] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 100, regs, 125,
                                 ReadTiming{/*transactionDeadlineMs=*/1000,
                                            /*responseTimeoutMs=*/100});

    TEST_ASSERT_EQUAL(ReadStatus::Timeout, r.status);
    // Bounded by the deadline rather than by luck: without it this loop never returns.
    TEST_ASSERT_TRUE(t.nowMs() >= 1000);
    TEST_ASSERT_TRUE(t.nowMs() < 5000);
}

// Guard added during the hoist: a caller asking for more registers than its buffer holds is
// refused outright rather than trusted, so an arithmetic slip cannot become an overrun driven
// by a byte count the device chose.
static void test_a_request_larger_than_the_buffer_is_refused_before_the_bus() {
    MockTransport t;
    t.replyWith(goodReply(kUnit, kReadHoldingRegisters, {1, 2, 3, 4}));

    uint16_t   regs[2] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 4, regs, 2);

    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
    TEST_ASSERT_TRUE(t.writes.empty());  // nothing was ever put on the bus
}

static void test_a_zero_length_read_is_refused() {
    MockTransport t;
    uint16_t      regs[1] = {};
    const auto    r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 0, regs, 1);

    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
    TEST_ASSERT_TRUE(t.writes.empty());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_good_reply_decodes);
    RUN_TEST(test_an_exception_reports_its_code);
    RUN_TEST(test_silence_is_a_timeout);
    RUN_TEST(test_a_corrupt_frame_is_protocol_not_timeout);
    RUN_TEST(test_a_reply_from_another_unit_is_refused);
    RUN_TEST(test_an_endless_trickle_still_hits_the_deadline);
    RUN_TEST(test_a_request_larger_than_the_buffer_is_refused_before_the_bus);
    RUN_TEST(test_a_zero_length_read_is_refused);
    return UNITY_END();
}
