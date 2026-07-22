// SPDX-License-Identifier: MIT
// Frame parsing, response validation and payload decoding.

#include <unity.h>

#include <string>

#include "drivers/eversolar_legacy/eversolar_parser.h"
#include "protocols/pmu/pmu_protocol.h"
#include "fixtures/eversolar_frames.h"

using namespace heliograph::eversolar;
namespace fx = heliograph::fixtures;

void setUp() {}
void tearDown() {}

static constexpr double kEps = 1e-9;
static const Address    kInverter = inverterAddress(0x10);

// --- framing ----------------------------------------------------------------------------

static void test_expected_frame_length_needs_the_length_byte() {
    TEST_ASSERT_EQUAL_size_t(0, expectedFrameLength(fx::kRespNormalInfoSingle, 4));
    TEST_ASSERT_EQUAL_size_t(0, expectedFrameLength(fx::kRespNormalInfoSingle, 8));
}

static void test_expected_frame_length_is_overhead_plus_payload() {
    const size_t n = expectedFrameLength(fx::kRespNormalInfoSingle, 9);
    TEST_ASSERT_EQUAL_size_t(kFrameOverhead + 28, n);
    TEST_ASSERT_EQUAL_size_t(fx::kRespNormalInfoSingleLen, n);
}

static void test_partial_frame_is_incomplete_not_invalid() {
    // A partial frame must never look like data. It is a "read more", not an error.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Incomplete, parseFrame(fx::kRespPartial, fx::kRespPartialLen, f));
}

static void test_truncated_before_length_byte_is_incomplete() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Incomplete, parseFrame(fx::kRespNormalInfoSingle, 5, f));
}

static void test_one_byte_is_incomplete() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Incomplete, parseFrame(fx::kRespNormalInfoSingle, 1, f));
}

static void test_bad_header_is_rejected() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::BadHeader, parseFrame(fx::kRespBadHeader, fx::kRespBadHeaderLen, f));
}

static void test_parse_extracts_addresses_and_codes() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespNormalInfoSingle, fx::kRespNormalInfoSingleLen, f));
    TEST_ASSERT_TRUE(f.source == kInverter);
    TEST_ASSERT_TRUE(f.destination == kPmuAddress);
    TEST_ASSERT_EQUAL_UINT8(0x11, f.control);
    TEST_ASSERT_EQUAL_UINT8(0x82, f.function);
    TEST_ASSERT_EQUAL_size_t(28, f.dataLength);
}

static void test_trailing_bytes_do_not_extend_the_frame() {
    // Extra bytes after a complete frame must be left for the next parse, not swallowed.
    uint8_t buf[fx::kRespNormalInfoSingleLen + 4];
    for (size_t i = 0; i < fx::kRespNormalInfoSingleLen; ++i) buf[i] = fx::kRespNormalInfoSingle[i];
    for (size_t i = 0; i < 4; ++i) buf[fx::kRespNormalInfoSingleLen + i] = 0xEE;

    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok, parseFrame(buf, sizeof(buf), f));
    TEST_ASSERT_EQUAL_size_t(fx::kRespNormalInfoSingleLen, f.frameLength);
}

// --- response validation -----------------------------------------------------------------

static void test_response_from_expected_inverter_is_accepted() {
    Frame f;
    parseFrame(fx::kRespNormalInfoSingle, fx::kRespNormalInfoSingleLen, f);
    TEST_ASSERT_EQUAL(ParseResult::Ok, validateResponse(f, cmd::kQueryNormalInfo, kInverter));
}

static void test_response_from_other_inverter_is_rejected() {
    // Checksum is valid; only the source address betrays it. The reference implementation
    // does not check this at all.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespWrongSource, fx::kRespWrongSourceLen, f));
    TEST_ASSERT_EQUAL(ParseResult::WrongSource,
                      validateResponse(f, cmd::kQueryNormalInfo, kInverter));
}

static void test_response_not_addressed_to_us_is_rejected() {
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespWrongDestination, fx::kRespWrongDestinationLen, f));
    TEST_ASSERT_EQUAL(ParseResult::WrongDestination,
                      validateResponse(f, cmd::kQueryNormalInfo, kInverter));
}

static void test_response_to_a_different_request_is_rejected() {
    Frame f;
    parseFrame(fx::kRespNormalInfoSingle, fx::kRespNormalInfoSingleLen, f);
    TEST_ASSERT_EQUAL(ParseResult::UnexpectedResponse,
                      validateResponse(f, cmd::kQueryInverterId, kInverter));
}

static void test_request_echo_is_not_mistaken_for_a_response() {
    // On a half-duplex bus our own request can be read back. Function 0x02 != 0x82.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kReqQueryNormalInfo, fx::kReqQueryNormalInfoLen, f));
    TEST_ASSERT_EQUAL(ParseResult::WrongDestination,
                      validateResponse(f, cmd::kQueryNormalInfo, kInverter));
}

static void test_response_function_is_request_function_with_bit7() {
    TEST_ASSERT_EQUAL_UINT8(0x82, cmd::kQueryNormalInfo.responseFunction());
    TEST_ASSERT_EQUAL_UINT8(0x80, cmd::kOfflineQuery.responseFunction());
    TEST_ASSERT_EQUAL_UINT8(0x83, cmd::kQueryInverterId.responseFunction());
}

// --- word helpers -------------------------------------------------------------------------

static void test_words_are_big_endian() {
    const uint8_t d[] = {0x12, 0x34, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT16(0x1234, readWord(d, 0));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, readWord(d, 1));
}

static void test_signed_word_handles_negatives() {
    const uint8_t d[] = {0xFF, 0xFF, 0x80, 0x00, 0x01, 0x9D};
    TEST_ASSERT_EQUAL_INT16(-1, readSignedWord(d, 0));       // -0.1 C
    TEST_ASSERT_EQUAL_INT16(-32768, readSignedWord(d, 1));
    TEST_ASSERT_EQUAL_INT16(413, readSignedWord(d, 2));      // 41.3 C
}

static void test_double_word_uses_65536_not_65535() {
    // The reference multiplies the high word by 65535. This is the whole reason our
    // energy.total differs from eversolar-monitor's; see docs/eversolar-protocol.md.
    const uint8_t d[] = {0x00, 0x02, 0xD0, 0xCF};  // hi=2, lo=53455
    TEST_ASSERT_EQUAL_UINT32(184527u, readDoubleWord(d, 0, 1));
    TEST_ASSERT_NOT_EQUAL(2u * 65535u + 53455u, readDoubleWord(d, 0, 1));
}

// --- single string layout ------------------------------------------------------------------

static void test_single_string_layout_is_detected_from_length() {
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::Ok,
                      decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData,
                                       kNormalInfoSingleStringBytes, info));
    TEST_ASSERT_EQUAL(NormalInfoLayout::SingleString, info.layout);
    TEST_ASSERT_FALSE(info.hasSecondString);
}

static void test_single_string_decodes_all_values() {
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, kNormalInfoSingleStringBytes, info);

    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kTemperatureC, info.temperatureC);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kEnergyTodayKwh, info.energyTodayKwh);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kEnergyTotalKwh, info.energyTotalKwh);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcVoltage, info.acVoltage);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcCurrent, info.acCurrent);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcPowerW, info.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kFrequencyHz, info.frequencyHz);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvVoltage1, info.pvVoltage1);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvCurrent1, info.pvCurrent1);
    TEST_ASSERT_EQUAL_UINT32(fx::expected::kOperatingHours, info.operatingHours);
    TEST_ASSERT_EQUAL_UINT16(fx::expected::kStatusCode, info.statusCode);
}

static void test_captured_44_byte_payload_decodes_as_single_string() {
    // The first QUERY_NORMAL_INFO a real TL3000-20 ever answered us (2026-07-19): a 44-byte
    // payload -- neither 28 nor 32. The first 14 words are exactly the single-string layout
    // (the values below cross-check physically: 655 W AC from 346.4 V x 2.0 A DC), followed
    // by an 8-word tail of zeros and FFFF. Auto must accept this; rejecting it was why the
    // first live poll returned invalid_frame while the inverter was answering perfectly.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespNormalInfoCaptured, fx::kRespNormalInfoCapturedLen, f));

    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::Ok, decodeNormalInfo(f.data, f.dataLength, info));
    TEST_ASSERT_EQUAL(NormalInfoLayout::SingleString, info.layout);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 38.6, info.temperatureC);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 3.44, info.energyTodayKwh);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 346.4, info.pvVoltage1);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 2.0, info.pvCurrent1);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 2.7, info.acCurrent);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 230.6, info.acVoltage);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 50.03, info.frequencyHz);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 655.0, info.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 35445.9, info.energyTotalKwh);
    TEST_ASSERT_EQUAL_UINT32(47401, info.operatingHours);
    TEST_ASSERT_EQUAL_UINT16(1, info.statusCode);
    TEST_ASSERT_FALSE(info.hasSecondString);
}

static void test_50_byte_payload_decodes_as_dual_generation() {
    // 50 bytes = the dual-string words plus a 9-word tail, per the calibrated Zeversolar
    // 2000s capture in ha-zeversolar-modbus. That unit has ONE MPPT: "dual" marks a protocol
    // generation, not a string count. Length-wise fixture is derived, offsets are theirs.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok, parseFrame(fx::kRespNormalInfoDualExtended,
                                                  fx::kRespNormalInfoDualExtendedLen, f));
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::Ok, decodeNormalInfo(f.data, f.dataLength, info));
    TEST_ASSERT_EQUAL(NormalInfoLayout::DualString, info.layout);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcPowerW, info.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kEnergyTotalKwh, info.energyTotalKwh);
}

static void test_ac_power_has_no_scale_factor() {
    // PAC is raw watts while everything around it is /10. Easy to get wrong.
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, kNormalInfoSingleStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 1842.0, info.acPowerW);
}

static void test_energy_today_and_total_use_different_scales() {
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, kNormalInfoSingleStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 8.42, info.energyTodayKwh);      // /100
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 18452.7, info.energyTotalKwh);   // /10
}

static void test_energy_total_differs_from_reference_by_exactly_the_known_bug() {
    // Documents the expected Phase 3 deviation against eversolar-monitor as a test, so it
    // cannot be mistaken for a regression later.
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, kNormalInfoSingleStringBytes, info);
    const double delta = info.energyTotalKwh - fx::expected::kEnergyTotalKwhReferenceBug;
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, fx::expected::kEnergyTotalExpectedDelta, delta);
}

static void test_impedance_is_reported_only_for_single_string() {
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, kNormalInfoSingleStringBytes, info);
    TEST_ASSERT_TRUE(info.impedanceValid);
}

// --- dual string layout ---------------------------------------------------------------------

static void test_dual_string_layout_is_detected_from_length() {
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::Ok,
                      decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData,
                                       kNormalInfoDualStringBytes, info));
    TEST_ASSERT_EQUAL(NormalInfoLayout::DualString, info.layout);
    TEST_ASSERT_TRUE(info.hasSecondString);
    TEST_ASSERT_FALSE(info.impedanceValid);
}

static void test_dual_string_decodes_both_mppts() {
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData, kNormalInfoDualStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvVoltage1, info.pvVoltage1);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvCurrent1, info.pvCurrent1);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvVoltage2, info.pvVoltage2);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kPvCurrent2, info.pvCurrent2);
}

static void test_dual_string_reads_the_same_physical_values_despite_field_order() {
    // IAC/VAC swap places and PAC moves between layouts. If the parser used one layout's
    // indices on the other's payload these would silently be wrong.
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData, kNormalInfoDualStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcVoltage, info.acVoltage);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcCurrent, info.acCurrent);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kAcPowerW, info.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kFrequencyHz, info.frequencyHz);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kEnergyTotalKwh, info.energyTotalKwh);
}

// --- night / edge cases ----------------------------------------------------------------------

static void test_night_frame_decodes_negative_temperature() {
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoNight + kOffsetData, kNormalInfoSingleStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, -0.1, info.temperatureC);
}

static void test_night_frame_zeros_are_real_measurements() {
    // A genuine 0 W at night must decode as 0 and stay distinguishable from "unknown",
    // which is expressed by validity flags rather than by the value.
    NormalInfo info;
    decodeNormalInfo(fx::kRespNormalInfoNight + kOffsetData, kNormalInfoSingleStringBytes, info);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 0.0, info.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, 0.0, info.energyTodayKwh);
    // Lifetime energy keeps counting even while nothing is produced.
    TEST_ASSERT_DOUBLE_WITHIN(kEps, fx::expected::kEnergyTotalKwh, info.energyTotalKwh);
}

// --- manual layout override -----------------------------------------------------------------

static void test_forced_layout_agrees_with_auto_detection() {
    NormalInfo autoInfo;
    NormalInfo forcedInfo;
    decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData, kNormalInfoDualStringBytes, autoInfo);
    TEST_ASSERT_EQUAL(DecodeResult::Ok,
                      decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData,
                                       kNormalInfoDualStringBytes, forcedInfo,
                                       LayoutSelection::ForceDualString));
    TEST_ASSERT_DOUBLE_WITHIN(kEps, autoInfo.acPowerW, forcedInfo.acPowerW);
    TEST_ASSERT_DOUBLE_WITHIN(kEps, autoInfo.pvVoltage2, forcedInfo.pvVoltage2);
}

static void test_forced_single_string_reads_a_dual_string_payload() {
    // The escape hatch for the unproven "length implies layout" assumption: if a device ever
    // sends 32 bytes while using the single-string field order, this is how it gets read.
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::Ok,
                      decodeNormalInfo(fx::kRespNormalInfoDual + kOffsetData,
                                       kNormalInfoDualStringBytes, info,
                                       LayoutSelection::ForceSingleString));
    TEST_ASSERT_EQUAL(NormalInfoLayout::SingleString, info.layout);
    TEST_ASSERT_FALSE(info.hasSecondString);
}

static void test_forcing_dual_string_on_a_short_payload_is_refused() {
    // Must not read past the payload. A 28-byte frame has no words 14/15.
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::LayoutMismatch,
                      decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData,
                                       kNormalInfoSingleStringBytes, info,
                                       LayoutSelection::ForceDualString));
}

static void test_forcing_a_layout_on_a_truncated_payload_is_refused() {
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::LayoutMismatch,
                      decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, 27, info,
                                       LayoutSelection::ForceSingleString));
}

static void test_forced_layout_accepts_an_unexpected_length() {
    // 30 bytes is rejected by Auto, but a forced layout reads the words it needs. This is the
    // whole point of the override: a device that contradicts the length hypothesis stays
    // usable without a firmware change.
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::UnknownLayout,
                      decodeNormalInfo(fx::kRespBadPayloadLength + kOffsetData, 30, info));
    TEST_ASSERT_EQUAL(DecodeResult::Ok,
                      decodeNormalInfo(fx::kRespBadPayloadLength + kOffsetData, 30, info,
                                       LayoutSelection::ForceSingleString));
}

static void test_unknown_payload_length_is_rejected() {
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::UnknownLayout,
                      decodeNormalInfo(fx::kRespBadPayloadLength + kOffsetData, 30, info));
    TEST_ASSERT_EQUAL(DecodeResult::UnknownLayout, decodeNormalInfo(nullptr, 0, info));
    TEST_ASSERT_EQUAL(DecodeResult::UnknownLayout,
                      decodeNormalInfo(fx::kRespNormalInfoSingle + kOffsetData, 27, info));
}

static void test_bad_payload_length_frame_parses_but_does_not_decode() {
    // The frame itself is structurally fine; only the payload size gives it away. Both
    // layers must reject it independently.
    Frame f;
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      parseFrame(fx::kRespBadPayloadLength, fx::kRespBadPayloadLengthLen, f));
    NormalInfo info;
    TEST_ASSERT_EQUAL(DecodeResult::UnknownLayout, decodeNormalInfo(f.data, f.dataLength, info));
}

// --- registration payloads ----------------------------------------------------------------

static void test_serial_number_is_extracted() {
    Frame f;
    parseFrame(fx::kRespOfflineQuery, fx::kRespOfflineQueryLen, f);
    // Source 00 00, not the to-be-assigned address: an unregistered inverter has no address
    // yet. Proven on hardware 2026-07-19; the constructed fixtures guessed wrong before that.
    TEST_ASSERT_EQUAL(ParseResult::Ok,
                      validateResponse(f, cmd::kOfflineQuery, kBroadcastAddress));

    std::string serial;
    TEST_ASSERT_TRUE(parseSerialNumber(f.data, f.dataLength, serial));
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedSerial, serial.c_str());
}

static void test_empty_or_unprintable_serial_is_refused() {
    // Refusing this prevents a garbage frame from registering as a device.
    std::string serial;
    TEST_ASSERT_FALSE(parseSerialNumber(nullptr, 0, serial));

    const uint8_t junk[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_FALSE(parseSerialNumber(junk, sizeof(junk), serial));

    const uint8_t control[] = {0x01, 0x02, 0x03};
    TEST_ASSERT_FALSE(parseSerialNumber(control, sizeof(control), serial));
}

static void test_registration_ack_is_recognised() {
    Frame f;
    parseFrame(fx::kRespRegisterAck, fx::kRespRegisterAckLen, f);
    TEST_ASSERT_TRUE(isRegistrationAck(f.data, f.dataLength));
}

static void test_registration_nak_is_not_an_ack() {
    Frame f;
    parseFrame(fx::kRespRegisterNak, fx::kRespRegisterNakLen, f);
    TEST_ASSERT_FALSE(isRegistrationAck(f.data, f.dataLength));
    TEST_ASSERT_FALSE(isRegistrationAck(nullptr, 0));
}

static void test_inverter_id_is_extracted_verbatim() {
    Frame f;
    parseFrame(fx::kRespInverterId, fx::kRespInverterIdLen, f);
    TEST_ASSERT_EQUAL(ParseResult::Ok, validateResponse(f, cmd::kQueryInverterId, kInverter));

    std::string id;
    TEST_ASSERT_TRUE(parseInverterId(f.data, f.dataLength, id));
    // The internal structure of this string is undocumented, so it is not split into
    // model/firmware fields here.
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedInverterId, id.c_str());
}

static void test_model_is_anchored_on_the_manufacturer_field() {
    // The real TL3000-20's id string (captured 2026-07-19). The model is the field directly
    // before the manufacturer -- the one field we can verify independently. Anything without
    // that anchor yields "" so the caller keeps the raw string instead of a guess.
    TEST_ASSERT_EQUAL_STRING(fx::kExpectedModel,
                             modelFromIdString(fx::kExpectedInverterId, "Ever-Solar").c_str());
    // The pre-hardware constructed guess had no manufacturer field at all: no anchor, no claim.
    TEST_ASSERT_EQUAL_STRING("", modelFromIdString("TL3000-20       3000E1.00", "Ever-Solar").c_str());
    // Manufacturer as the first field has nothing before it to call a model.
    TEST_ASSERT_EQUAL_STRING("", modelFromIdString("Ever-Solar  TL3000-20", "Ever-Solar").c_str());
    TEST_ASSERT_EQUAL_STRING("", modelFromIdString("", "Ever-Solar").c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_expected_frame_length_needs_the_length_byte);
    RUN_TEST(test_expected_frame_length_is_overhead_plus_payload);
    RUN_TEST(test_partial_frame_is_incomplete_not_invalid);
    RUN_TEST(test_truncated_before_length_byte_is_incomplete);
    RUN_TEST(test_one_byte_is_incomplete);
    RUN_TEST(test_bad_header_is_rejected);
    RUN_TEST(test_parse_extracts_addresses_and_codes);
    RUN_TEST(test_trailing_bytes_do_not_extend_the_frame);
    RUN_TEST(test_response_from_expected_inverter_is_accepted);
    RUN_TEST(test_response_from_other_inverter_is_rejected);
    RUN_TEST(test_response_not_addressed_to_us_is_rejected);
    RUN_TEST(test_response_to_a_different_request_is_rejected);
    RUN_TEST(test_request_echo_is_not_mistaken_for_a_response);
    RUN_TEST(test_response_function_is_request_function_with_bit7);
    RUN_TEST(test_words_are_big_endian);
    RUN_TEST(test_signed_word_handles_negatives);
    RUN_TEST(test_double_word_uses_65536_not_65535);
    RUN_TEST(test_single_string_layout_is_detected_from_length);
    RUN_TEST(test_single_string_decodes_all_values);
    RUN_TEST(test_captured_44_byte_payload_decodes_as_single_string);
    RUN_TEST(test_50_byte_payload_decodes_as_dual_generation);
    RUN_TEST(test_ac_power_has_no_scale_factor);
    RUN_TEST(test_energy_today_and_total_use_different_scales);
    RUN_TEST(test_energy_total_differs_from_reference_by_exactly_the_known_bug);
    RUN_TEST(test_impedance_is_reported_only_for_single_string);
    RUN_TEST(test_dual_string_layout_is_detected_from_length);
    RUN_TEST(test_dual_string_decodes_both_mppts);
    RUN_TEST(test_dual_string_reads_the_same_physical_values_despite_field_order);
    RUN_TEST(test_night_frame_decodes_negative_temperature);
    RUN_TEST(test_night_frame_zeros_are_real_measurements);
    RUN_TEST(test_forced_layout_agrees_with_auto_detection);
    RUN_TEST(test_forced_single_string_reads_a_dual_string_payload);
    RUN_TEST(test_forcing_dual_string_on_a_short_payload_is_refused);
    RUN_TEST(test_forcing_a_layout_on_a_truncated_payload_is_refused);
    RUN_TEST(test_forced_layout_accepts_an_unexpected_length);
    RUN_TEST(test_unknown_payload_length_is_rejected);
    RUN_TEST(test_bad_payload_length_frame_parses_but_does_not_decode);
    RUN_TEST(test_serial_number_is_extracted);
    RUN_TEST(test_model_is_anchored_on_the_manufacturer_field);
    RUN_TEST(test_empty_or_unprintable_serial_is_refused);
    RUN_TEST(test_registration_ack_is_recognised);
    RUN_TEST(test_registration_nak_is_not_an_ack);
    RUN_TEST(test_inverter_id_is_extracted_verbatim);
    return UNITY_END();
}
