// SPDX-License-Identifier: MIT

#include "app/device_plan.h"

namespace heliograph::app {

std::string describeRow(size_t row, const std::string& label) {
    const std::string base = "device " + std::to_string(row);
    return label.empty() ? base : base + " (" + label + ")";
}

std::vector<PlannedDevice> planDevices(const Configuration& config, const std::string& driverId,
                                       const DriverRegistry& registry) {
    std::vector<PlannedDevice> planned;

    // Device 1 comes from `driver`, the rest from `additional_devices`, in that order. One list
    // so the poll loop has one thing to walk, and so a bring-up log reads in the same order the
    // settings page shows.
    //
    // driverId is passed in rather than read from the config: it is the EFFECTIVE id, which may
    // come from auto-detection or from what is compiled in, and resolving that needs the
    // registry state at boot rather than the file on flash.
    if (!driverId.empty()) {
        planned.push_back({driverId, config.driver.label, &config.driver.options,
                           planned.size() + 1, ""});
    }
    for (const auto& device : config.additionalDevices) {
        planned.push_back(
            {device.id, device.label, &device.options, planned.size() + 1, ""});
    }

    // Refused BEFORE anything is created or begun, because for a driver that cannot share a bus
    // begin() is itself the harm.
    //
    // Asked of the REGISTRY, not of the configuration: whether a driver can share a bus is a
    // property of the driver, and the config layer deliberately knows nothing about drivers.
    //
    // The FIRST occurrence wins and later ones are refused. That is what makes the order above
    // load-bearing: it decides which of two identically-configured devices gets the bus.
    for (size_t i = 0; i < planned.size(); ++i) {
        const auto* descriptor = registry.find(planned[i].id);
        if (descriptor == nullptr || descriptor->supportsMultipleDevices) {
            continue;
        }
        for (size_t earlier = 0; earlier < i; ++earlier) {
            // Only an earlier device that is itself STARTING can claim the bus: a refused one
            // never calls begin(), so it can never be the reason a later one is refused.
            //
            // Today this changes nothing -- the loop stops at the first match, which is always
            // the one that started -- so it is a guard against a future second reason for
            // refusing a device, not a fix for a bug that exists. Said plainly because a
            // condition whose comment claims a failure it does not prevent is worse than no
            // comment (review).
            if (planned[earlier].id == planned[i].id && planned[earlier].shouldStart()) {
                planned[i].problem = describeRow(planned[i].row, planned[i].label) + " ('" +
                                     planned[i].id +
                                     "') was not started: this driver supports only one device "
                                     "per bridge";
                break;
            }
        }
    }
    return planned;
}

}  // namespace heliograph::app
