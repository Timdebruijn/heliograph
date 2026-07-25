// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace heliograph::mqtt {

/// One device as it was last announced to the broker, with the topic tree it was announced on.
///
/// The id alone is not enough. A device's topics are a function of (id, primary): the primary
/// device publishes on the bridge-scoped tree it has always used, every other device on
/// `<base>/<bridgeId>/device/<id>/`. A device that keeps its id but moves between those two
/// trees leaves the one it came from behind, and without this flag nothing could tell.
struct AnnouncedDevice {
    std::string id;
    bool        primary = false;
};

/// Which previously announced devices left a DEVICE-SCOPED topic tree that nothing publishes to
/// any more, and whose retained payloads therefore have to be cleared.
///
/// Two cases, both of which put a device's entities in Home Assistant forever with their last
/// value (availability is bridge-scoped by design, so an orphan never goes unavailable):
///   - it is gone from the configuration, or its address changed, which changes its id;
///   - it was promoted into the `driver` slot, so it publishes on the bridge-scoped tree now
///     and its old per-device tree has no owner.
///
/// The bridge-scoped tree is never cleared, and that is deliberate rather than an omission.
/// It is either owned by the device that is primary now, or it is about to be inherited by
/// the next one -- which is exactly the back-compat contract: an existing install's entities
/// and their recorder history survive the first device being re-addressed or replaced.
/// Clearing it would delete the live primary's entities. The residue is the discovery configs
/// of measurements the old primary published and the new one does not; docs/mqtt.md says so.
///
/// Pure and host-testable on purpose: MqttOutput is compiled only for ESP32, so a rule that
/// lived inside it could not be tested at all. The previous shape of this decision -- a
/// `primary` flag computed at the call site from a variable that could never hold a removed
/// device's id -- was provably dead and no test could have seen it (review, 2026-07-26).
inline std::vector<std::string> devicesToForget(const std::vector<AnnouncedDevice>& announced,
                                                const std::vector<std::string>&     current,
                                                const std::string&                  primaryNow) {
    std::vector<std::string> out;
    for (const auto& a : announced) {
        if (a.primary) {
            continue;
        }
        const bool stillDeviceScoped =
            a.id != primaryNow &&
            std::find(current.begin(), current.end(), a.id) != current.end();
        if (!stillDeviceScoped &&
            std::find(out.begin(), out.end(), a.id) == out.end()) {
            out.push_back(a.id);
        }
    }
    return out;
}

}  // namespace heliograph::mqtt
