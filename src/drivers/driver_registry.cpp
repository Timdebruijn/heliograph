// SPDX-License-Identifier: MIT

#include "driver_registry.h"

#include <cstdlib>

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
#if ENABLE_DRIVER_SUNSPEC
#include "sunspec/sunspec_driver.h"
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
        const DriverOption* option = descriptor.findOption(key);
        if (option == nullptr) {
            error = {key, "unknown option for driver '" + descriptor.id + "'"};
            return false;
        }
        if (option->isNumeric()) {
            // Refused, not clamped: a value the user typed and a value we invented must not
            // both end up stored as if they were the same decision.
            // strtol skips leading whitespace and accepts a leading '+' and leading zeros, so
            // " 7", "+7" and "007" all parse -- and were then STORED verbatim, which is how
            // "007" slipped past a duplicate-address check that compares strings. Rejected
            // rather than normalised: silently rewriting what someone typed is the same class
            // of substitution this bound exists to remove (review, 2026-07-25).
            const bool clean = !value.empty() &&
                               value.find_first_not_of("0123456789") == std::string::npos &&
                               (value.size() == 1 || value[0] != '0');
            char*      end    = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (!clean || end == value.c_str() || *end != '\0') {
                error = {key, "must be a whole number between " + std::to_string(option->minValue) +
                                  " and " + std::to_string(option->maxValue)};
                return false;
            }
            if (parsed < option->minValue || parsed > option->maxValue) {
                error = {key, "must be between " + std::to_string(option->minValue) + " and " +
                                  std::to_string(option->maxValue)};
                return false;
            }
            continue;
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

DriverRegistry::Entry* DriverRegistry::findEntry(const std::string& driverId) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.descriptor.id == driverId;
    });
    return it == entries_.end() ? nullptr : &*it;
}

const DriverRegistry::Entry* DriverRegistry::findEntry(const std::string& driverId) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.descriptor.id == driverId;
    });
    return it == entries_.end() ? nullptr : &*it;
}

void DriverRegistry::registerDriver(const DriverDescriptor& descriptor, DriverFactory factory) {
    if (Entry* existing = findEntry(descriptor.id); existing != nullptr) {
        existing->descriptor = descriptor;
        existing->factory    = std::move(factory);
        return;
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
    const Entry* entry = findEntry(driverId);
    return entry == nullptr ? nullptr : &entry->descriptor;
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
#if ENABLE_DRIVER_SUNSPEC
    registry.registerDriver(sunspec::descriptor(), sunspec::factory);
#endif
#if ENABLE_DRIVER_MOCK
    registry.registerDriver(mock::readOnlyDescriptor(), mock::readOnlyFactory);
    registry.registerDriver(mock::writableDescriptor(), mock::writableFactory);
#endif
    (void)registry;
}

}  // namespace heliograph
