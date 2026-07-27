// SPDX-License-Identifier: MIT
//
// REST response bodies. Pure: no web server, no Arduino.

#pragma once

#include <string>
#include <vector>

#include "app/capture_runner.h"
#include "config/config_backup.h"
#include "device/bridge_info.h"
#include "device/device_state.h"
#include "diagnostics/diagnostics.h"
#include "app/discovery_runner.h"
#include "drivers/driver_descriptor.h"

namespace heliograph::rest {

inline constexpr size_t kMaxResponseBytes = 8192;
/// Bodies above this are refused with 413 before parsing, so a large POST cannot exhaust the
/// heap just by arriving. Routes that legitimately need more say so per route -- see
/// kMaxBackupBytes, which a configuration restore passes to collectBody.
inline constexpr size_t kMaxRequestBytes = 4096;

struct ApiError {
    int         httpStatus;
    std::string code;
    std::string message;
};

/// Uniform error body. Every failure looks the same, and a failure is never a 200.
bool buildErrorPayload(const ApiError& error, const std::string& requestId, std::string& out);

/// Response to a completed setup-portal provision. Echoes the hostname because the AP is gone
/// after the reboot and the user needs a concrete address; the setup page turns it into a link.
/// A builder rather than a hand-spliced string: the hostname is operator-supplied, and the page
/// puts it straight into innerHTML, so it must be escaped by the same path as every other
/// payload no matter how strict today's validation happens to be.
bool buildProvisionPayload(const std::string& hostname, std::string& out,
                           size_t maxBytes = kMaxResponseBytes);

/// One polled device, reduced to what an always-visible summary needs.
///
/// Exists so the Dashboard and the header indicator can describe every inverter from the one
/// request they already make. Walking /api/v1/devices/<id> per device would be 1+N requests per
/// second on the board that is also driving the RS485 bus -- the Device tab does that, and has
/// to rate-limit itself to once every five seconds because of it.
///
/// Every channel is optional and says so separately from its value: a device that does not
/// report energy must not contribute a zero to a total.
struct DeviceSummary {
    std::string id;
    /// The Modbus TCP unit id this device is served at, or 0 when Modbus is off or this device
    /// is past the end of the served run. Reported because otherwise the only way to learn it
    /// is to read modbus.unit_id, fetch the device list and count positions -- and on a bus of
    /// identical inverters that is exactly the sum nobody wants to do twice (review).
    uint8_t     modbusUnitId = 0;
    bool        online    = false;
    bool        dataValid = false;
    bool        dataStale = false;
    bool        everPolled          = false;
    uint32_t    lastPollSecondsAgo  = 0;
    bool        hasAcPower       = false;
    double      acPowerW         = 0.0;
    bool        hasEnergyToday   = false;
    double      energyTodayKwh   = 0.0;
    bool        hasEnergyTotal   = false;
    double      energyTotalKwh   = 0.0;
    /// Per-device only: these two are never summed. A household total of volts is meaningless
    /// and a total of temperatures worse, so unlike the three above they exist purely so the
    /// fleet strip can show whether one inverter sits at a different voltage or runs hotter
    /// than its neighbours -- which on a shared bus is the interesting comparison.
    bool        hasAcVoltage     = false;
    double      acVoltageV       = 0.0;
    bool        hasTemperature   = false;
    double      temperatureC     = 0.0;
    /// Hybrids only, and absent on every string inverter. `batteryPowerW` keeps the project's
    /// sign convention (measurement.h): positive charging, negative discharging -- so direction
    /// and magnitude travel as one number rather than as a flag that can disagree with it.
    bool        hasBatterySoc    = false;
    double      batterySocPct    = 0.0;
    bool        hasBatteryPower  = false;
    double      batteryPowerW    = 0.0;
};

/// What a fleet adds up to.
///
/// A count travels with every sum because a total over none is unknown, not zero -- and a total
/// over two of three inverters must not read as the household figure.
struct FleetTotals {
    unsigned polled        = 0;
    /// Started, online, holding data that is valid and NOT stale. The strict reading: a device
    /// that never returned a byte is not answering, and neither is one whose reading aged out.
    unsigned answering     = 0;
    unsigned acCount       = 0;
    double   acPowerW      = 0.0;
    unsigned todayCount    = 0;
    double   energyTodayKwh = 0.0;
    unsigned lifetimeCount = 0;
    double   energyTotalKwh = 0.0;
};

/// Sums a fleet. Pure, so the status payload and the log heartbeat can agree by construction
/// rather than by two people remembering the same rule -- the heartbeat used to describe the
/// first device only, and there was no shared definition of "answering" for it to use (#74).
FleetTotals totalsFor(const std::vector<DeviceSummary>& fleet);

/// Reduces one device's state to the row above.
///
/// A reading counts only while it is valid AND fresh -- the same rule every other output uses.
/// A stale one is dropped rather than carried: `markAllStale()` keeps `valid` true when a
/// device goes offline, so carrying it means a dead inverter's last daylight value stays in the
/// household total until the next reboot. The row still reports how long ago the device
/// answered, which is what the reading has been replaced by.
DeviceSummary summariseDevice(const DeviceState& state, const std::string& deviceId,
                              uint64_t nowMs);

/// `deviceId` is the id the device is REGISTERED under -- the key in /api/v1/devices and the
/// per-device routes. Not identity.deviceId(): that one changes when a late-arriving serial
/// number completes the identity (the store key was minted at begin(), before registration on
/// the bus), and reporting it sent clients to a path that 404s. Seen live 2026-07-19.
///
/// `fleet` is every polled device, first one included, in registration order. It drives the
/// Dashboard totals and the header indicator; `device` stays the first device so that every
/// existing client keeps working unchanged.
bool buildStatusPayload(const DeviceState& state, const std::string& deviceId,
                        const BridgeInfo& bridge, const DiagnosticsSnapshot& diagnostics,
                        const DriverDescriptor* driver, uint64_t nowMs,
                        const std::vector<DeviceSummary>& fleet, std::string& out,
                        size_t maxBytes = kMaxResponseBytes);

bool buildDevicesPayload(const std::vector<std::string>& deviceIds, std::string& out,
                         size_t maxBytes = kMaxResponseBytes);

/// `nowMs` is only used to age the last successful poll. Without it this payload said whether a
/// device was online and never when it last answered -- and for every device but the first,
/// this is the ONLY payload there is, so "started but has never returned a byte" and "working"
/// were indistinguishable outside the status endpoint (review, 2026-07-25).
bool buildDevicePayload(const DeviceState& state, const std::string& deviceId,
                        const DriverDescriptor* driver, uint64_t nowMs, std::string& out,
                        size_t maxBytes = kMaxResponseBytes);

bool buildMeasurementsPayload(const DeviceState& state, std::string& out,
                              size_t maxBytes = kMaxResponseBytes);

bool buildDiagnosticsPayload(const DiagnosticsSnapshot& diagnostics, const BridgeInfo& bridge,
                             std::string& out, size_t maxBytes = kMaxResponseBytes);

/// The discovery report, with every piece of evidence §28 asks the wizard to show: which
/// driver and serial profile were tried, whether anything answered, whether the checksum held,
/// the confidence score, and the reason a match was or was not accepted automatically.
bool buildDiscoveryPayload(const DiscoveryReport& report, uint64_t nowMs, std::string& out,
                           size_t maxBytes = kMaxResponseBytes);

/// Drivers compiled into this build. The discovery wizard needs this rather than a hardcoded
/// list in the frontend.
bool buildDriversPayload(const std::vector<DriverDescriptor>& drivers, std::string& out,
                         size_t maxBytes = kMaxResponseBytes);

/// Recent log lines, oldest first. Bounded separately: a hex-dump log is bulkier than any
/// other response, and truncating it silently would defeat the point of reading it.
///
/// Sized from the worst case, not a round number: 64 ring lines x 255 chars each plus JSON
/// quoting/array overhead is ~17 KB. The first pick (16384) was exceeded exactly when the
/// ring was full of maximum-length TRACE lines -- a 500 at the moment the tool matters most.
inline constexpr size_t kMaxLogResponseBytes = 24576;
/// `level` is the active log level name, echoed in the payload. It answers the question a
/// reader otherwise cannot: "why is there no TRACE data in here?" -- because the level says
/// info, not because the bus is silent. Added for driver bring-up sessions, where the raw
/// dumps are TRACE-only and the default level would silently hide them.
bool buildLogsPayload(const std::vector<std::string>& lines, uint32_t totalLines,
                      const std::string& level, std::string& out,
                      size_t maxBytes = kMaxLogResponseBytes);

/// A raw bus capture, for a device nothing in this build can identify.
///
/// Bounded separately and generously: this is hex, which is two characters per byte before JSON
/// quoting, and the default 64x256 window can produce well over the 8 KB a normal response is
/// held to. Truncating the one artefact a contributor is going to paste into an issue would
/// defeat the feature.
inline constexpr size_t kMaxCaptureResponseBytes = 65536;
bool buildCapturePayload(const CaptureReport& report, uint64_t nowMs, std::string& out,
                         size_t maxBytes = kMaxCaptureResponseBytes);
/// What a restore would do, before it does it.
///
/// The preview is computed against the MERGED result, not against the file, so it tells the
/// truth about a redacted backup: those credentials show as unchanged because leaving them
/// alone is what applying it will actually do.
///
/// `rollbackExists` is whether an undo point is stored RIGHT NOW, from an earlier restore.
///
/// Not "will an undo exist afterwards", which is what this field claimed until a review caught
/// it: that cannot be known before the copy is attempted, because the attempt is the only thing
/// that discovers whether it fits. The result payload's `rollback_stored` answers that, after
/// the fact, truthfully.
///
/// What this one is for is the case where an undo point already exists: applying is about to
/// REPLACE it, so the way back becomes this configuration rather than the older one. That is a
/// thing someone can get wrong, and nothing else says it.
///
/// A separate response bound: a restore that touches every setting produces a long table, and
/// truncating the one screen the operator is using to decide would defeat it.
inline constexpr size_t kMaxRestorePreviewBytes = 16384;
bool buildRestorePreviewPayload(const BackupContents& backup,
                                const std::vector<ConfigDiffEntry>& diff, bool rebootRequired,
                                bool rollbackExists, std::string& out,
                                size_t maxBytes = kMaxRestorePreviewBytes);

/// The outcome of an applied restore. `rollbackStored` false means the undo copy did not fit;
/// the restore still happened, and saying so is the whole point of reporting it separately.
bool buildRestoreResultPayload(size_t changedFields, bool rebootRequired, bool rollbackStored,
                               std::string& out, size_t maxBytes = kMaxResponseBytes);

}  // namespace heliograph::rest
