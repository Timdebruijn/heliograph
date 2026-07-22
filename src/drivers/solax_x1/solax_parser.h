// SPDX-License-Identifier: MIT
//
// SolaX X1 series payload decoding (PMU family framing, see protocols/pmu/pmu_protocol.h).
// Platform independent: no Arduino / ESP-IDF headers.
//
// Protocol knowledge is derived from syssi/esphome-solax-x1-mini (Apache-2.0, hardware-
// tested against X1 Mini G1/G2/G3) and the official "SolaxPower Single Phase External
// Communication Protocol X1 Series V1.2" document. No code was copied; the decoding was
// re-implemented. STATUS: transcribed, NOT yet confirmed against a real X1 Mini by this
// project -- an X1-1.1-S-D bring-up session is planned. Endianness of the two 32-bit
// fields is the least certain part (see the notes at each field).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "protocols/pmu/pmu_protocol.h"

namespace heliograph::solax {

// Framing is shared PMU; the names are used unqualified here just as the other family
// driver does.
using namespace ::heliograph::pmu;

/// Status report payload lengths seen per hardware generation (reference implementation):
/// G2 sends 50 bytes, G1 sends 52 (adds CT Pgrid), G3 sends 56. The first 50 bytes are
/// layout-identical across generations, so decoding accepts any payload of at least 50
/// bytes and reads only that common prefix.
inline constexpr size_t kStatusReportMinBytes = 50;

/// Registration serial number: 14 raw bytes announced in the offline-query response and
/// echoed back verbatim in the address assignment.
inline constexpr size_t kSerialNumberBytes = 14;

/// Device info payload (0x11 0x83): fixed 58-byte layout of ASCII fields.
inline constexpr size_t kDeviceInfoBytes = 58;

/// Decoded status report, already scaled to physical units.
///
/// The grid/PV fault-threshold words (offsets 32-44) are deliberately not exposed: they
/// describe protection limits, not measurements, and their meaning per generation is
/// unverified. They stay visible in the TRACE payload dump for the bring-up session.
struct StatusReport {
    double temperatureC   = 0.0;  ///< offset 0, int16, 1 °C
    double energyTodayKwh = 0.0;  ///< offset 2, u16, /10
    double pv1Voltage     = 0.0;  ///< offset 4, u16, /10
    double pv2Voltage     = 0.0;  ///< offset 6, u16, /10
    double pv1Current     = 0.0;  ///< offset 8, u16, /10
    double pv2Current     = 0.0;  ///< offset 10, u16, /10
    double acCurrent      = 0.0;  ///< offset 12, u16, /10
    double acVoltage      = 0.0;  ///< offset 14, u16, /10
    double frequencyHz    = 0.0;  ///< offset 16, u16, /100
    double acPowerW       = 0.0;  ///< offset 18, u16, whole watts
    /// offset 22, 32-bit, /10. Read big-endian like every 16-bit field; UNPROVEN against
    /// hardware -- if lifetime energy looks absurd on the bench, this is the first suspect.
    double energyTotalKwh = 0.0;
    /// offset 26, 32-bit big-endian. Same endianness caveat as energyTotalKwh.
    uint32_t runtimeHours = 0;
    uint16_t mode         = 0;  ///< offset 30: 0 Wait, 1 Check, 2 Normal, 3 Fault, ...
    /// offset 46, 32-bit error bitmask. The reference reads this one LITTLE-endian --
    /// opposite to everything else in the payload -- so that is what we do. Zero = no fault.
    uint32_t errorBits = 0;
};

enum class DecodeResult {
    Ok,
    TooShort,  ///< payload shorter than the 50-byte common prefix
};

/// Decodes a status report (0x11 0x82) payload. Accepts >= kStatusReportMinBytes; longer
/// generation-specific tails are ignored (and visible in the TRACE dump).
DecodeResult decodeStatusReport(const uint8_t* data, size_t len, StatusReport& out);

/// Operating mode as text. Returns "" for a mode outside the documented 0-6 table, so an
/// unknown mode is never published with an invented name.
const char* modeText(uint16_t mode);

/// Decoded device info (0x11 0x83): fixed-width space-padded ASCII fields.
struct DeviceInfo {
    uint8_t     deviceType = 0;   ///< offset 0, raw byte
    std::string ratedPower;       ///< offsets 1-6
    std::string firmwareVersion;  ///< offsets 7-11
    std::string moduleName;       ///< offsets 12-25
    std::string factoryName;      ///< offsets 26-39
    std::string serialNumber;     ///< offsets 40-53
};

/// Decodes a device info payload. Requires exactly kDeviceInfoBytes.
bool decodeDeviceInfo(const uint8_t* data, size_t len, DeviceInfo& out);

/// True when the 14-byte registration serial looks plausible (at least one printable,
/// non-space byte), so a garbage frame cannot register as a device.
bool serialLooksValid(const uint8_t* data, size_t len);

}  // namespace heliograph::solax
