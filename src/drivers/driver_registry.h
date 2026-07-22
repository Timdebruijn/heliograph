// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "driver_descriptor.h"
#include "inverter_driver.h"

namespace heliograph {

/// Builds a driver instance. The options are the user-configured driver settings
/// (DriverSettings::options, validated by validateDriverOptions); factories parse them via
/// their optionsFrom() so a configured unit_id/profile/layout actually takes effect.
using DriverFactory =
    std::function<std::unique_ptr<InverterDriver>(Transport&, const DriverOptions&)>;

/// Holds the drivers compiled into this firmware.
///
/// Adding a driver means registering it here and nothing else: no output adapter, no REST
/// handler and no web page may need touching. That property is what the registry exists for.
class DriverRegistry {
public:
    /// Later registrations of the same id replace earlier ones.
    void registerDriver(const DriverDescriptor& descriptor, DriverFactory factory);

    /// Descriptors, sorted by probePriority descending then id, so discovery order and the
    /// UI listing are deterministic.
    std::vector<DriverDescriptor> availableDrivers() const;

    const DriverDescriptor* find(const std::string& driverId) const;
    bool                    contains(const std::string& driverId) const;
    size_t                  size() const { return entries_.size(); }

    /// Returns nullptr for an unknown id, or when the driver does not support this
    /// transport type. `options` are the configured driver settings; omit for factory
    /// defaults (discovery probes with defaults on purpose: it identifies hardware, and
    /// must not inherit a stale configuration from a previously selected driver).
    std::unique_ptr<InverterDriver> create(const std::string& driverId, Transport& transport,
                                           const DriverOptions& options = {}) const;

private:
    struct Entry {
        DriverDescriptor descriptor;
        DriverFactory    factory;
    };
    std::vector<Entry> entries_;
};

/// Registers every driver enabled at compile time. Flash is finite, so which drivers exist
/// is a build-time decision (ENABLE_DRIVER_*), not a runtime one.
void registerBuiltinDrivers(DriverRegistry& registry);

}  // namespace heliograph
