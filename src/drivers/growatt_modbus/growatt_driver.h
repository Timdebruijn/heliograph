// SPDX-License-Identifier: MIT
//
// Growatt hybrid/string inverter driver, Modbus RTU master.
//
// Read-only and Experimental until validated on an SPH6000. The register map lives in
// growatt_registers.{h,cpp}; this file does the Modbus IO and orchestration only. Uses the
// shared src/protocols/modbus codec -- no brand-specific framing.
//
// See docs/growatt-sph-protocol.md. Battery *control* is deliberately not implemented yet:
// writing holding registers on a hybrid with an unvalidated map could move real energy. Reads
// are non-destructive and immediately checkable against the inverter display, so they come
// first; writes follow only after the map is confirmed.

#pragma once

#include <memory>

#include "drivers/growatt_modbus/growatt_registers.h"
#include "drivers/inverter_driver.h"
#include "drivers/modbus_bus_tally.h"

namespace heliograph::growatt {

const DriverDescriptor& descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

struct GrowattOptions {
    /// Modbus slave/unit id. Growatt default is 1.
    uint8_t unitId = 1;
    /// Which register-map profile to use. Profiles are data (profiles/growatt/*.toml), not
    /// code paths, so MIN TL-XH / MOD / MIC slot in as new TOML files.
    const GrowattProfile* profile = &defaultProfile();
};

GrowattOptions optionsFrom(const heliograph::DriverOptions& values);

class GrowattDriver : public InverterDriver {
public:
    explicit GrowattDriver(Transport& transport, GrowattOptions options = {});

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    DeviceIdentity          identity() const override;
    InverterCapabilities    capabilities() const override;

    /// Always Unsupported for now: this driver is read-only until the register map is confirmed
    /// on hardware. Battery control is a deliberate later step, not an oversight.
    CommandResult execute(const InverterCommand& command) override;

    BusErrorCounts busErrors() const override { return busErrors_; }

private:
    /// The write row that can actually serve this command, or nullptr.
    ///
    /// Used by BOTH capabilities() and execute() so the two can never disagree about which
    /// setpoints exist. A driver that advertises a command and then refuses it is worse than
    /// one that never offered it.
    const WriteMapping* writeFor(InverterCommandType type) const;

    /// Crc is separate from Protocol for the same reason it is in the codec: it is the only one
    /// that means the wire is bad, and it is what the checksum-error metric and its alert key on.
    enum class ReadResult { Ok, Timeout, Exception, Crc, Protocol, TransportError };

    /// One Modbus read transaction into `out` (at least `count` words). Sets lastException_ on
    /// an exception reply. TRACE-dumps the raw block for hardware bring-up. `probe` marks a
    /// block read only to answer a bring-up question; its failures never reach the bus counters.
    ReadResult readBlock(RegSpace space, uint16_t start, uint16_t count, uint16_t* out,
                         bool probe = false);

    Transport*     transport_ = nullptr;
    GrowattOptions options_;
    uint8_t        lastException_ = 0;
    BusErrorCounts busErrors_{};

    static constexpr size_t kMaxBlocks = 8;
    /// Scratch for poll(): ~2 KB. A member, not a stack array, on purpose. The rs485 task runs
    /// on an 8 KB stack that was sized for the EverSolar TRACE incident; measured on the real
    /// binary, poll()'s frame with this array on the stack plus the TRACE logging chain peaked
    /// at ~5.2 KB (64%) -- too close to a second stack-canary boot loop. The driver lives on
    /// the heap and rs485Task owns the bus exclusively, so a member is safe.
    /// Value-initialised so the FIRST poll cannot decode indeterminate memory. It is only that:
    /// from the second poll on these slots hold whatever the previous poll left, and because a
    /// slot is claimed by index as blocks_[validCount] a skipped block can leave a neighbour's
    /// data behind it. What actually keeps stale words out of a reading is readRegisters()
    /// refusing a reply shorter than requested, plus applyProfile() only walking [0, validCount).
    /// This initialiser is a floor under the first poll, not the guarantee.
    BlockData blocks_[kMaxBlocks]{};
};

}  // namespace heliograph::growatt
