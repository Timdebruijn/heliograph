// SPDX-License-Identifier: MIT
//
// Profile-driven inverter driver, Modbus RTU master.
//
// One driver for every device whose register map fits in a table: the map lives in
// profile_tables.{h,cpp}, generated from profiles/<vendor>/*.toml, and this file does the
// Modbus IO and orchestration only. Uses the shared src/protocols/modbus codec -- no
// vendor-specific framing anywhere.
//
// Writing is possible but doubly gated: a profile must declare a [[write]] row AND that row
// must be marked verified against real hardware. Writing holding registers on a hybrid with an
// unvalidated map moves real energy, so reads -- non-destructive and immediately checkable
// against the inverter's own display -- always come first. See
// docs/device-profiles/write-path.md.

#pragma once

#include <memory>

#include "drivers/modbus_profile/profile_tables.h"
#include "drivers/inverter_driver.h"
#include "drivers/modbus_bus_tally.h"

namespace heliograph::profile {

const DriverDescriptor& descriptor();
std::unique_ptr<InverterDriver> factory(Transport& transport, const DriverOptions& options);

struct ProfileOptions {
    /// Modbus slave/unit id. Most vendors ship 1; the profile's notes say when a family differs.
    uint8_t unitId = 1;
    /// Which register-map profile to use. Profiles are data (profiles/<vendor>/*.toml), not code
    /// paths, so a new family or a new brand slots in as a new TOML file.
    const DeviceProfile* profile = &defaultProfile();
};

ProfileOptions optionsFrom(const heliograph::DriverOptions& values);

class ModbusProfileDriver : public InverterDriver {
public:
    explicit ModbusProfileDriver(Transport& transport, ProfileOptions options = {});

    const DriverDescriptor& descriptor() const override;
    bool                    begin(Transport& transport) override;
    ProbeResult             probe() override;
    PollResult              poll(DeviceState& state) override;
    DeviceIdentity          identity() const override;
    InverterCapabilities    capabilities() const override;

    /// Writes the holding register the profile names for this command, over FC06.
    ///
    /// Unsupported unless writeFor() finds a row it can actually serve: no [[write]] row for
    /// this command, a row still marked unverified, or a row needing more than one register.
    /// That is the normal answer today -- no profile in the tree has a verified row yet.
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
    ProfileOptions options_;
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

}  // namespace heliograph::profile
