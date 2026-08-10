// SPDX-License-Identifier: MIT
//
// SolarMax driver against a mock device that speaks MaxTalk. The framing itself is covered in
// test_maxtalk; what is asserted here is the brand half -- which code fills which channel, what
// it is divided by, and what the driver does when a device answers only part of what it asked.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "drivers/solarmax/solarmax_driver.h"
#include "protocols/maxtalk/maxtalk.h"
#include "support/mock_transport.h"

using namespace heliograph;
using heliograph::test::MockTransport;

namespace {

/// Builds a well-formed reply carrying `pairs`, addressed from `from` to the host. Kept in the
/// test rather than in the transport, so the mock stays protocol-agnostic.
std::string makeReply(uint8_t from, const std::string& pairs) {
    const std::string body  = "|64:" + pairs + "|";
    char              head[16];
    const size_t      total = 9 + body.size() + 5;
    std::snprintf(head, sizeof(head), "%02X;FB;%02X", from, static_cast<unsigned>(total));
    const std::string inner = std::string(head) + body;
    char              sum[8];
    std::snprintf(sum, sizeof(sum), "%04X", maxtalk::checksum(inner.c_str(), inner.size()));
    return "{" + inner + std::string(sum) + "}";
}

void respondWith(MockTransport& t, uint8_t from, const std::string& pairs) {
    const std::string reply = makeReply(from, pairs);
    t.setResponder([reply](const std::vector<uint8_t>&, std::vector<uint8_t>& out) {
        out.assign(reply.begin(), reply.end());
        return true;
    });
}

/// Everything a healthy single-phase unit would answer.
const char* kFullSinglePhase =
    "PAC=1770;PDC=1810;UL1=08FC;IL1=01F4;TNF=1388;TKK=2A;"
    "UD01=0BB8;ID01=015E;KDY=7B;KT0=2710;KHR=1388;SYS=4E20;SAL=0";

double value(const DeviceState& s, const char* id) {
    const auto* m = s.measurements.find(id);
    return m != nullptr ? m->value : -1.0;
}

bool has(const DeviceState& s, const char* id) {
    const auto* m = s.measurements.find(id);
    return m != nullptr && m->valid;
}

}  // namespace

void setUp() {}
void tearDown() {}

// The scaling table, asserted as arithmetic rather than as "it produced a number". Every divisor
// here is the weak part of this driver -- most rest on a single source -- so the test pins what
// the code currently claims, and a hardware session that disagrees will fail it loudly.
static void test_a_full_reply_fills_the_canonical_channels_with_the_documented_scaling() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    TEST_ASSERT_TRUE(d.begin(t));
    respondWith(t, 0x05, kFullSinglePhase);

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(s));

    TEST_ASSERT_EQUAL_DOUBLE(0x1770 / 2.0, value(s, measurement_id::kAcPowerTotal));
    TEST_ASSERT_EQUAL_DOUBLE(0x1810 / 2.0, value(s, measurement_id::kDcPowerTotal));
    TEST_ASSERT_EQUAL_DOUBLE(0x08FC / 10.0, value(s, measurement_id::kAcL1Voltage));
    TEST_ASSERT_EQUAL_DOUBLE(0x01F4 / 100.0, value(s, measurement_id::kAcL1Current));
    TEST_ASSERT_EQUAL_DOUBLE(0x1388 / 100.0, value(s, measurement_id::kAcFrequency));
    TEST_ASSERT_EQUAL_DOUBLE(0x2A, value(s, measurement_id::kTemperature));
    TEST_ASSERT_EQUAL_DOUBLE(0x0BB8 / 10.0, value(s, measurement_id::kDcMppt1Voltage));
    TEST_ASSERT_EQUAL_DOUBLE(0x015E / 100.0, value(s, measurement_id::kDcMppt1Current));
    TEST_ASSERT_EQUAL_DOUBLE(0x7B / 10.0, value(s, measurement_id::kEnergyToday));
    TEST_ASSERT_EQUAL_DOUBLE(0x2710, value(s, measurement_id::kEnergyTotal));
    TEST_ASSERT_EQUAL_DOUBLE(0x1388, value(s, measurement_id::kOperatingHours));
}

// Status and fault land on DeviceState rather than in the measurement set, and both are marked
// supported so an output publishes the number instead of null. The status TEXT stays a bare
// rendering of the code: neither source enumerates the table, and inventing words for it would
// be a guess wearing a label.
static void test_status_and_alarm_are_reported_without_inventing_meanings() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, kFullSinglePhase);

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(s));
    TEST_ASSERT_TRUE(s.statusCodeSupported);
    TEST_ASSERT_EQUAL_UINT16(0x4E20, s.statusCode);
    TEST_ASSERT_TRUE(s.errorCodeSupported);
    TEST_ASSERT_EQUAL_UINT32(0, s.errorCode);
    TEST_ASSERT_TRUE(s.statusText.find("20000") != std::string::npos);
}

// A single-phase unit does not answer UL2/UL3, and that must read as absent rather than zero --
// zero volts on L2 is a real measurement and would look like a dead phase.
static void test_channels_a_device_does_not_answer_stay_absent_rather_than_zero() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, kFullSinglePhase);

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(s));

    TEST_ASSERT_TRUE(has(s, measurement_id::kAcL1Voltage));
    TEST_ASSERT_FALSE(has(s, measurement_id::kAcL2Voltage));
    TEST_ASSERT_FALSE(has(s, measurement_id::kAcL3Voltage));
    TEST_ASSERT_FALSE(has(s, measurement_id::kDcMppt2Voltage));
    TEST_ASSERT_EQUAL_UINT8(1, d.capabilities().phaseCount);
    TEST_ASSERT_EQUAL_UINT8(1, d.capabilities().mpptCount);
}

// The declared shape follows what the device answered, not what the constructor hoped.
static void test_a_three_phase_reply_raises_the_declared_phase_count() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05,
                "PAC=1770;UL1=08FC;UL2=0906;UL3=08F0;IL1=01F4;IL2=01E0;IL3=0200;"
                "UD01=0BB8;UD02=0BC2;KDY=7B");

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(s));
    TEST_ASSERT_EQUAL_UINT8(3, d.capabilities().phaseCount);
    TEST_ASSERT_EQUAL_UINT8(2, d.capabilities().mpptCount);
    TEST_ASSERT_TRUE(has(s, measurement_id::kAcL3Voltage));
}

// DC voltage is deliberately unmapped: the two sources disagree about UDC's scale, so publishing
// either reading would be picking a winner where the project's own rule says not to. A device
// that answers UDC must therefore show no DC voltage rather than a plausible wrong one.
static void test_the_contested_dc_voltage_code_is_not_published() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, "PAC=1770;UDC=0BB8;IDC=015E;KDY=7B");

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Ok, d.poll(s));

    TEST_ASSERT_TRUE(has(s, measurement_id::kAcPowerTotal));
    TEST_ASSERT_FALSE(has(s, measurement_id::kDcMppt1Voltage));
    TEST_ASSERT_FALSE(has(s, measurement_id::kDcMppt1Current));
}

// Another unit's reply is not ours. On a shared bus this is ordinary traffic, and decoding it
// would attribute one inverter's production to another.
static void test_a_reply_from_a_different_address_is_not_accepted() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x06, kFullSinglePhase);  // unit 6 answering while we asked unit 5

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, d.poll(s));
    TEST_ASSERT_FALSE(has(s, measurement_id::kAcPowerTotal));
}

static void test_silence_is_a_timeout_and_leaves_the_state_untouched() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    t.clearResponder();

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::Timeout, d.poll(s));
    TEST_ASSERT_FALSE(has(s, measurement_id::kAcPowerTotal));
    TEST_ASSERT_TRUE(d.busErrors().timeouts > 0);
}

// A corrupted reply moves the checksum counter and nothing else. That counter is the one metric
// that indicts the cabling, so a fault of any other kind must not touch it.
static void test_a_corrupted_reply_counts_against_the_wire_and_not_the_device() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);

    std::string bad = makeReply(0x05, "PAC=1770;KDY=7B");
    bad[20]         = (bad[20] == '1') ? '2' : '1';
    t.setResponder([bad](const std::vector<uint8_t>&, std::vector<uint8_t>& out) {
        out.assign(bad.begin(), bad.end());
        return true;
    });

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::ChecksumError, d.poll(s));
    TEST_ASSERT_EQUAL_UINT32(1, d.busErrors().checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, d.busErrors().invalidFrames);
}

// A frame that parses but carries nothing we map is not a successful poll. Returning Ok there
// would publish an empty measurement set as current data.
static void test_a_reply_with_no_mapped_codes_is_not_a_successful_poll() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, "XYZ=1;ABC=2");

    DeviceState s;
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, d.poll(s));
}

// The probe asks a question and changes nothing: no address is assigned, no mode is entered. That
// is what lets discovery treat this family as read-only, unlike the AA55 drivers beside it.
// The poll contract: state is modified ONLY when returning Ok. A reply carrying nothing this
// driver maps must leave the caller's retained state exactly as it found it -- no declared
// channels, no capability shape, no status word.
static void test_an_unusable_reply_leaves_the_state_completely_untouched() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, "XYZ=1;ABC=2");

    DeviceState s;
    s.statusText = "untouched";
    TEST_ASSERT_EQUAL(PollResult::InvalidFrame, d.poll(s));

    TEST_ASSERT_EQUAL_size_t(0, s.measurements.size());
    TEST_ASSERT_FALSE(s.statusCodeSupported);
    TEST_ASSERT_FALSE(s.errorCodeSupported);
    TEST_ASSERT_EQUAL_STRING("untouched", s.statusText.c_str());
}

static void test_the_probe_identifies_without_writing_anything_but_a_query() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x05, "TYP=64;SWV=1F4");

    const ProbeResult r = d.probe();
    TEST_ASSERT_TRUE(r.responded);
    TEST_ASSERT_TRUE(r.checksumValid);
    TEST_ASSERT_EQUAL_STRING("SolarMax", r.detectedManufacturer.c_str());
    TEST_ASSERT_TRUE(r.confidenceScore > 0);
    // Hex on the wire, hex in the label: "type 0x64", not the decimal 100.
    TEST_ASSERT_EQUAL_STRING("type 0x64", r.detectedModel.c_str());

    // Exactly one frame went out, and it is a query: it opens with the host address and carries
    // the payload marker. Nothing in this protocol writes, and nothing here should.
    TEST_ASSERT_EQUAL_size_t(1, t.writes.size());
    const std::string sent(t.writes[0].begin(), t.writes[0].end());
    TEST_ASSERT_EQUAL_size_t(0, sent.find("{FB;05;"));
    TEST_ASSERT_TRUE(sent.find("|64:TYP;SWV|") != std::string::npos);
}

static void test_a_silent_bus_probes_as_no_device_rather_than_a_broken_one() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    t.clearResponder();

    const ProbeResult r = d.probe();
    TEST_ASSERT_FALSE(r.responded);
    TEST_ASSERT_FALSE(r.sawTraffic);
    TEST_ASSERT_EQUAL_INT(0, r.confidenceScore);
}

// Traffic from another address during a sweep is the duplicate-address diagnosis, not a failed
// probe -- the same distinction the ProbeResult contract draws for the Modbus drivers.
static void test_another_devices_frame_during_a_probe_is_recorded_as_traffic() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);
    respondWith(t, 0x09, "TYP=64;SWV=1F4");

    const ProbeResult r = d.probe();
    TEST_ASSERT_FALSE(r.responded);
    TEST_ASSERT_TRUE(r.sawTraffic);
}

static void test_the_driver_refuses_every_write() {
    MockTransport t;
    solarmax::SolarmaxDriver d(t, solarmax::SolarmaxOptions{0x05});
    d.begin(t);

    InverterCommand c;
    c.type = InverterCommandType::SetActivePowerLimitWatts;
    c.numericValue = 50.0;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, d.execute(c));

    c.type = InverterCommandType::Stop;
    TEST_ASSERT_EQUAL(CommandResult::Unsupported, d.execute(c));

    // And it never claims otherwise, so the dispatcher's capability gate refuses before reaching
    // execute() at all.
    for (size_t i = 0; i < static_cast<size_t>(InverterCapability::_Count); ++i) {
        TEST_ASSERT_FALSE(d.capabilities().canWrite(static_cast<InverterCapability>(i)));
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_full_reply_fills_the_canonical_channels_with_the_documented_scaling);
    RUN_TEST(test_status_and_alarm_are_reported_without_inventing_meanings);
    RUN_TEST(test_channels_a_device_does_not_answer_stay_absent_rather_than_zero);
    RUN_TEST(test_a_three_phase_reply_raises_the_declared_phase_count);
    RUN_TEST(test_the_contested_dc_voltage_code_is_not_published);
    RUN_TEST(test_a_reply_from_a_different_address_is_not_accepted);
    RUN_TEST(test_silence_is_a_timeout_and_leaves_the_state_untouched);
    RUN_TEST(test_a_corrupted_reply_counts_against_the_wire_and_not_the_device);
    RUN_TEST(test_a_reply_with_no_mapped_codes_is_not_a_successful_poll);
    RUN_TEST(test_an_unusable_reply_leaves_the_state_completely_untouched);
    RUN_TEST(test_the_probe_identifies_without_writing_anything_but_a_query);
    RUN_TEST(test_a_silent_bus_probes_as_no_device_rather_than_a_broken_one);
    RUN_TEST(test_another_devices_frame_during_a_probe_is_recorded_as_traffic);
    RUN_TEST(test_the_driver_refuses_every_write);
    return UNITY_END();
}
