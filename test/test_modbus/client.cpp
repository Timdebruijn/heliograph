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
#include "support/modbus_frame.h"

using namespace heliograph;
using namespace heliograph::modbus;
using heliograph::test::MockTransport;


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
    test::appendModbusCrc(f);
    return f;
}

std::vector<uint8_t> exceptionReply(uint8_t unit, uint8_t fn, uint8_t code) {
    std::vector<uint8_t> f{unit, static_cast<uint8_t>(fn | 0x80), code};
    test::appendModbusCrc(f);
    return f;
}

/// The echo a device sends back for 0x06: the address and the value it accepted, verbatim.
std::vector<uint8_t> writeEcho(uint8_t unit, uint16_t address, uint16_t value) {
    std::vector<uint8_t> f{unit,
                           kWriteSingleRegister,
                           static_cast<uint8_t>(address >> 8),
                           static_cast<uint8_t>(address & 0xFF),
                           static_cast<uint8_t>(value >> 8),
                           static_cast<uint8_t>(value & 0xFF)};
    test::appendModbusCrc(f);
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

static void test_a_corrupt_frame_is_crc_not_timeout_and_not_protocol() {
    MockTransport t;
    auto bad = goodReply(kUnit, kReadHoldingRegisters, {0x1234});
    bad.back() ^= 0xFF;  // wreck the CRC
    t.replyWith(bad);

    uint16_t   regs[1] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 1, regs, 1);

    // Three outcomes, three different things to go check in the field. Timeout: nothing came
    // back at all -- wiring, address, a sleeping device. Crc: bytes came back corrupted, which
    // indicts the wire itself (ground, termination, a stub, noise from the inverter). Protocol:
    // an intact frame that was not what we asked for -- addressing or a device quirk.
    //
    // Crc used to be folded into Protocol, and the drivers mapped Protocol to InvalidFrame, so
    // PollResult::ChecksumError was structurally unreachable on a Modbus bus and the alert rule
    // built on that counter could never fire (review, 2026-07-25).
    TEST_ASSERT_EQUAL(ReadStatus::Crc, r.status);
}

static void test_a_reply_from_another_unit_is_protocol_not_crc() {
    MockTransport t;
    t.replyWith(goodReply(kUnit + 1, kReadHoldingRegisters, {0x1111}));  // valid CRC, wrong unit

    uint16_t   regs[1] = {};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 1, regs, 1);

    // Protocol, deliberately not Crc: the frame arrived intact, a neighbour on the multidrop
    // bus simply answered. Nothing wrong with the cable, so it must not be counted against it.
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

// A CRC-valid reply that simply carries FEWER registers than were asked for is not success.
// The codec only bounds the byte count against the caller's capacity, so a short frame leaves
// the tail of the buffer untouched -- while the driver's block descriptor already claims the
// full count. Growatt's scratch is a member array (indeterminate on the first poll before it
// was value-initialised) and SunSpec zero-fills, where a zero scale factor is legal; either
// way the untouched tail would be decoded and published as genuine readings on a poll that
// reports Ok. Found in review 2026-07-25.
static void test_a_reply_with_too_few_registers_is_refused() {
    MockTransport t;
    t.replyWith(goodReply(kUnit, kReadHoldingRegisters, {0x1111, 0x2222}));  // 2, not 4

    uint16_t   regs[4] = {0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD};
    const auto r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 4, regs, 4);

    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
    // Whatever the caller had there stays there, and the caller is told not to trust it.
    TEST_ASSERT_EQUAL_UINT16(0xDEAD, regs[2]);
    TEST_ASSERT_EQUAL_UINT16(0xDEAD, regs[3]);
}

// The exact-length reply must still be Ok -- the guard above must not cost a legitimate read.
static void test_a_reply_with_exactly_the_requested_count_is_ok() {
    MockTransport t;
    t.replyWith(goodReply(kUnit, kReadInputRegisters, {1, 2, 3, 4}));

    uint16_t   regs[4] = {};
    const auto r = readRegisters(t, kUnit, kReadInputRegisters, kFrom, 4, regs, 4);

    TEST_ASSERT_EQUAL(ReadStatus::Ok, r.status);
    TEST_ASSERT_EQUAL_UINT16(4, regs[3]);
}

static void test_a_zero_length_read_is_refused() {
    MockTransport t;
    uint16_t      regs[1] = {};
    const auto    r = readRegisters(t, kUnit, kReadHoldingRegisters, kFrom, 0, regs, 1);

    TEST_ASSERT_EQUAL(ReadStatus::Protocol, r.status);
    TEST_ASSERT_TRUE(t.writes.empty());
}

// THE WRITE PATH HAD NO TESTS AT ALL. Every case below is new, and the reason they are worth
// having is the one the header states: a write whose echo is not verified is a request, not a
// setting. Mutation testing found this by replacing the echo comparison with `accepted = true`
// and watching the whole suite stay green -- nothing here called writeSingleRegister, while two
// production drivers do (modbus_profile and sunspec).
static void test_a_write_that_echoes_what_was_sent_is_a_setting() {
    MockTransport t;
    t.replyWith(writeEcho(kUnit, 0x0410, 750));

    const auto r = writeSingleRegister(t, kUnit, 0x0410, 750);

    TEST_ASSERT_EQUAL(TransactionStatus::Ok, r.status);
}

// The two that matter. A device that answers about a DIFFERENT register, or with a different
// value than the one asked for, has not done what was asked however well-formed the frame is.
// Reported as Protocol -- "intact, but not what we asked for" -- and never as Ok, because on a
// control register the difference is between an inverter that is limited and one everybody
// believes is limited.
static void test_a_write_echoing_another_address_is_not_a_setting() {
    MockTransport t;
    t.replyWith(writeEcho(kUnit, 0x0411, 750));  // neighbouring register

    const auto r = writeSingleRegister(t, kUnit, 0x0410, 750);

    TEST_ASSERT_EQUAL(TransactionStatus::Protocol, r.status);
}

static void test_a_write_echoing_another_value_is_not_a_setting() {
    MockTransport t;
    t.replyWith(writeEcho(kUnit, 0x0410, 1000));  // clamped by the device, or stale

    const auto r = writeSingleRegister(t, kUnit, 0x0410, 750);

    TEST_ASSERT_EQUAL(TransactionStatus::Protocol, r.status);
}

// A refusal is the ORDINARY answer to a control write -- read-only register, value out of
// range, a device wanting an unlock first -- so it carries its code rather than collapsing
// into a protocol error.
static void test_a_refused_write_carries_its_exception_code() {
    MockTransport t;
    t.replyWith(exceptionReply(kUnit, kWriteSingleRegister, 0x04));  // slave device failure

    const auto r = writeSingleRegister(t, kUnit, 0x0410, 750);

    TEST_ASSERT_EQUAL(TransactionStatus::Exception, r.status);
    TEST_ASSERT_EQUAL_UINT8(0x04, r.exceptionCode);
}

// Silence must never read as "written". This is the failure an unpowered or mis-addressed
// device produces, and it is the one most likely to be met in the field.
static void test_a_write_nobody_answers_is_a_timeout() {
    MockTransport t;
    t.replyWithSilence();

    const auto r = writeSingleRegister(t, kUnit, 0x0410, 750);

    TEST_ASSERT_EQUAL(TransactionStatus::Timeout, r.status);
}

void run_modbus_client() {
    RUN_TEST(test_a_good_reply_decodes);
    RUN_TEST(test_an_exception_reports_its_code);
    RUN_TEST(test_silence_is_a_timeout);
    RUN_TEST(test_a_corrupt_frame_is_crc_not_timeout_and_not_protocol);
    RUN_TEST(test_a_reply_from_another_unit_is_protocol_not_crc);
    RUN_TEST(test_an_endless_trickle_still_hits_the_deadline);
    RUN_TEST(test_a_request_larger_than_the_buffer_is_refused_before_the_bus);
    RUN_TEST(test_a_reply_with_too_few_registers_is_refused);
    RUN_TEST(test_a_reply_with_exactly_the_requested_count_is_ok);
    RUN_TEST(test_a_zero_length_read_is_refused);
    RUN_TEST(test_a_write_that_echoes_what_was_sent_is_a_setting);
    RUN_TEST(test_a_write_echoing_another_address_is_not_a_setting);
    RUN_TEST(test_a_write_echoing_another_value_is_not_a_setting);
    RUN_TEST(test_a_refused_write_carries_its_exception_code);
    RUN_TEST(test_a_write_nobody_answers_is_a_timeout);
}
