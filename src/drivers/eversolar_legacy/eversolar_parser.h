// SPDX-License-Identifier: MIT
//
// EverSolar payload decoding. See protocols/pmu/pmu_protocol.h for provenance.
// Platform independent: no Arduino / ESP-IDF headers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "protocols/pmu/pmu_protocol.h"

namespace heliograph::eversolar {

// Framing hoisted to protocols/pmu (2026-07-21); see the note in eversolar_driver.h.
using namespace ::heliograph::pmu;

/// Payload layout of QUERY_NORMAL_INFO (0x11 0x82).
///
/// The reference implementation makes the user choose this in a config file. We derive it
/// from the payload length instead: the two layouts differ in field *order*, so a wrong
/// choice yields plausible-looking nonsense rather than an obvious failure.
///
/// UNPROVEN HYPOTHESIS: that single-string firmware sends 28 bytes and dual-string firmware
/// sends 32. It follows from the reference's two 14- and 16-entry index maps, but the
/// reference itself makes the layout a config option, which is weak evidence against the
/// length being decisive. Use LayoutSelection to override if a real device disagrees.
enum class NormalInfoLayout {
    SingleString,  ///< 14 words / 28 bytes
    DualString,    ///< 16 words / 32 bytes
};

/// How to pick the layout. Auto is correct for every device seen so far; the forced values
/// exist so that a device contradicting the hypothesis above can still be read without a
/// firmware change.
enum class LayoutSelection {
    Auto,
    ForceSingleString,
    ForceDualString,
};

inline constexpr size_t kNormalInfoSingleStringBytes = 28;
inline constexpr size_t kNormalInfoDualStringBytes   = 32;
/// A real TL3000-20 (single string) sends 44 bytes: the 14 single-string words followed by an
/// 8-word tail of zeros and 0xFFFF, meaning unknown. Captured 2026-07-19; this is what finally
/// explains the reference making `strings` a config option -- it indexes words and ignores
/// everything after, so payload length never was decisive.
inline constexpr size_t kNormalInfoSingleStringExtendedBytes = 44;
/// A Zeversolar 2000s sends 50 bytes in the dual-string word order (calibrated capture in the
/// ha-zeversolar-modbus project, 2026-05-30: IAC/VAC/PAC at words 6/7/9, uint32 E_TOTAL at
/// 11/12, status at 15, cross-checked I*V==P). Note that unit has ONE MPPT: the "dual" layout
/// marks a protocol generation, not a string count -- its MPPT2 words simply read constant.
inline constexpr size_t kNormalInfoDualStringExtendedBytes = 50;

/// Decoded measurements, already scaled to their physical units.
///
/// Fields the protocol does not carry are absent rather than zero. `impedanceRaw` and the
/// NA_* words are deliberately not exposed as measurements: their unit and scale are unknown
/// (see docs/eversolar-protocol.md), and publishing them as zero would be a lie.
struct NormalInfo {
    NormalInfoLayout layout = NormalInfoLayout::SingleString;

    double temperatureC    = 0.0;  ///< TEMP, signed, /10
    double energyTodayKwh  = 0.0;  ///< E_TODAY, /100
    double energyTotalKwh  = 0.0;  ///< uint32(E_TOTAL_HI, E_TOTAL_LO), /10
    double acVoltage       = 0.0;  ///< VAC, /10
    double acCurrent       = 0.0;  ///< IAC, /10
    double acPowerW        = 0.0;  ///< PAC, unscaled watts
    double frequencyHz     = 0.0;  ///< FREQUENCY, /100

    double pvVoltage1 = 0.0;  ///< VPV, /10
    double pvCurrent1 = 0.0;  ///< IPV, /10

    bool   hasSecondString = false;  ///< true only for the dual-string layout
    double pvVoltage2      = 0.0;    ///< VPV2, /10
    double pvCurrent2      = 0.0;    ///< IPV2, /10

    uint32_t operatingHours = 0;  ///< HOURS_UP
    uint16_t statusCode     = 0;  ///< OP_MODE, raw. Meaning is undocumented; do not map to text.

    /// Only present in the single-string layout, at word 8. Unit and scale unknown.
    bool     impedanceValid = false;
    uint16_t impedanceRaw   = 0;
};

enum class DecodeResult {
    Ok,
    UnknownLayout,   ///< Auto: payload length is neither 28 nor 32 bytes
    LayoutMismatch,  ///< a forced layout needs more words than the payload carries
};

/// Reads a big-endian uint16 at word index `index`.
uint16_t readWord(const uint8_t* data, size_t index);

/// Reads a big-endian int16 at word index `index`. Used for TEMP.
int16_t readSignedWord(const uint8_t* data, size_t index);

/// Combines two words into a uint32 (high word first).
///
/// The reference implementation computes `low/10 + high*65535/10`, which is off by one
/// bit-width: the multiplier must be 65536. Our value is therefore high*0.1 kWh larger
/// than eversolar-monitor's. That difference is expected, not a bug — see
/// docs/eversolar-protocol.md and the Phase 3 validation tolerances.
uint32_t readDoubleWord(const uint8_t* data, size_t highIndex, size_t lowIndex);

/// Decodes a QUERY_NORMAL_INFO payload.
///
/// With LayoutSelection::Auto, `len` must be exactly 28 or 32. A forced layout only requires
/// the payload to be long enough, so a device sending an unexpected length can still be read
/// once someone has established which field order it really uses.
DecodeResult decodeNormalInfo(const uint8_t* data, size_t len, NormalInfo& out,
                              LayoutSelection selection = LayoutSelection::Auto);

/// Extracts the serial number from an OFFLINE_QUERY response (0x10 0x80).
///
/// The payload is the raw serial as ASCII. The reference unpacks it as a NUL-terminated
/// string and strips non-word characters. We additionally require that the payload contains
/// at least one printable character, so that a garbage frame cannot register as a device.
bool parseSerialNumber(const uint8_t* data, size_t len, std::string& out);

/// True if a SEND_REGISTER_ADDRESS response (0x10 0x81) acknowledges the registration.
bool isRegistrationAck(const uint8_t* data, size_t len);

/// Extracts the identification string from a QUERY_INVERTER_ID response (0x11 0x83).
/// The internal structure of this string is not documented; it is returned verbatim
/// (trimmed of padding) rather than parsed into model/firmware fields.
bool parseInverterId(const uint8_t* data, size_t len, std::string& out);

/// OP_MODE code -> observed meaning, or "" for a code never seen on hardware (the caller
/// then presents "Unknown (n)" -- naming an unobserved code would be a fabrication).
///
/// The code space is vendor-undocumented; every entry here is backed by measurement:
///   1 = grid-connected/producing. Two independent sources: our own capture (2026-07-19,
///       code 1 throughout grid-tied production on a TL3000-20) and the calibrated
///       Zeversolar 2000s capture in ha-zeversolar-modbus.
///   0 = standby, answering but not feeding. Four independent events in the HA history:
///       dusk 2026-07-19 22:20, 2026-07-20 22:30, 2026-07-21 22:27 (each: code 0 for the
///       last minute(s) before the inverter powers off), and dawn 2026-07-22 06:01 (code 0
///       for four minutes, then code 1 as production started).
const char* opModeText(uint16_t code);

/// Picks the bare model out of a raw inverter-id string, or "" when it cannot.
///
/// A real TL3000-20 sends (captured 2026-07-19, space-padded fields):
///   "1  3000E1.00   TL3000-20    Ever-Solar      XH300060115506193600V610-01023-06"
/// Using that whole string as the model made every Home Assistant name unreadable. The field
/// ORDER is a single-sample observation, so this does not parse by position: it splits on runs
/// of two or more spaces and anchors on the one field we know independently -- the
/// manufacturer name -- taking the field directly before it as the model. No anchor match, no
/// claim: the caller keeps the raw string.
std::string modelFromIdString(const std::string& idString, const std::string& manufacturer);

}  // namespace heliograph::eversolar
