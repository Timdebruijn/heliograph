// SPDX-License-Identifier: MIT

#include "driver_registry.h"

#include <algorithm>

#if ENABLE_DRIVER_EVERSOLAR
#include "eversolar_legacy/eversolar_driver.h"
#endif
#if ENABLE_DRIVER_GROWATT
#include "growatt_modbus/growatt_driver.h"
#endif
#if ENABLE_DRIVER_SOLAX
#include "solax_x1/solax_driver.h"
#endif
#if ENABLE_DRIVER_MOCK
#include "mock/mock_driver.h"
#endif

namespace heliograph {

const char* supportLevelName(DriverSupportLevel level) {
    switch (level) {
        case DriverSupportLevel::Experimental: return "experimental";
        case DriverSupportLevel::Beta:         return "beta";
        case DriverSupportLevel::Stable:       return "stable";
        case DriverSupportLevel::Deprecated:   return "deprecated";
    }
    return "unknown";
}

const char* pollResultName(PollResult result) {
    switch (result) {
        case PollResult::Ok:             return "ok";
        case PollResult::Timeout:        return "timeout";
        case PollResult::ChecksumError:  return "checksum_error";
        case PollResult::InvalidFrame:   return "invalid_frame";
        case PollResult::NotRegistered:  return "not_registered";
        case PollResult::TransportError: return "transport_error";
    }
    return "unknown";
}

bool validateDriverOptions(const DriverDescriptor& descriptor, const DriverOptions& values,
                           DriverOptionError& error) {
    for (const auto& [key, value] : values) {
        const DriverOption* option = nullptr;
        for (const auto& o : descriptor.options) {
            if (o.key == key) {
                option = &o;
                break;
            }
        }
        if (option == nullptr) {
            error = {key, "unknown option for driver '" + descriptor.id + "'"};
            return false;
        }
        if (option->allowedValues.empty()) {
            continue;  // free-form
        }
        bool ok = false;
        std::string allowed;
        for (const auto& v : option->allowedValues) {
            if (v == value) {
                ok = true;
            }
            allowed += (allowed.empty() ? "" : ", ") + v;
        }
        if (!ok) {
            error = {key, "must be one of: " + allowed};
            return false;
        }
    }
    return true;
}

void DriverRegistry::registerDriver(const DriverDescriptor& descriptor, DriverFactory factory) {
    for (auto& e : entries_) {
        if (e.descriptor.id == descriptor.id) {
            e.descriptor = descriptor;
            e.factory    = std::move(factory);
            return;
        }
    }
    entries_.push_back(Entry{descriptor, std::move(factory)});
}

std::vector<DriverDescriptor> DriverRegistry::availableDrivers() const {
    std::vector<DriverDescriptor> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        out.push_back(e.descriptor);
    }
    std::sort(out.begin(), out.end(), [](const DriverDescriptor& a, const DriverDescriptor& b) {
        if (a.probePriority != b.probePriority) {
            return a.probePriority > b.probePriority;
        }
        return a.id < b.id;
    });
    return out;
}

const DriverDescriptor* DriverRegistry::find(const std::string& driverId) const {
    for (const auto& e : entries_) {
        if (e.descriptor.id == driverId) {
            return &e.descriptor;
        }
    }
    return nullptr;
}

bool DriverRegistry::contains(const std::string& driverId) const {
    return find(driverId) != nullptr;
}

std::unique_ptr<InverterDriver> DriverRegistry::create(const std::string&   driverId,
                                                       Transport&           transport,
                                                       const DriverOptions& options) const {
    for (const auto& e : entries_) {
        if (e.descriptor.id != driverId) {
            continue;
        }
        // Refuse to build a driver on a bus it cannot speak over, rather than let it fail
        // later in a way that looks like a wiring problem.
        const auto& supported = e.descriptor.supportedTransports;
        if (std::find(supported.begin(), supported.end(), transport.type()) == supported.end()) {
            return nullptr;
        }
        return e.factory(transport, options);
    }
    return nullptr;
}

void registerBuiltinDrivers(DriverRegistry& registry) {
#if ENABLE_DRIVER_EVERSOLAR
    registry.registerDriver(eversolar::descriptor(), eversolar::factory);
#endif
#if ENABLE_DRIVER_GROWATT
    registry.registerDriver(growatt::descriptor(), growatt::factory);
#endif
#if ENABLE_DRIVER_SOLAX
    registry.registerDriver(solax::descriptor(), solax::factory);
#endif
#if ENABLE_DRIVER_MOCK
    registry.registerDriver(mock::readOnlyDescriptor(), mock::readOnlyFactory);
    registry.registerDriver(mock::writableDescriptor(), mock::writableFactory);
#endif
    (void)registry;
}

}  // namespace heliograph
