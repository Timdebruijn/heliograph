// SPDX-License-Identifier: MIT
//
// The single point where device state crosses between tasks.
//
// Uses std::mutex rather than a FreeRTOS semaphore: the Arduino-ESP32 toolchain implements it
// over pthreads, so the same code runs on the host under env:native. A store that can only be
// tested on hardware is a store that will not be tested.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "device/device_state.h"

namespace heliograph {

using DeviceId    = std::string;
using StateHandle = std::shared_ptr<const DeviceState>;

/// Holds the current state of one device.
///
/// Readers get an immutable snapshot and may hold it as long as they like without blocking
/// the writer: a slow REST client cannot delay the RS485 poll. The writer builds a fresh
/// DeviceState and swaps the pointer under the mutex, so readers never see a half-written
/// value.
class StateStore {
public:
    StateStore();

    void publish(const DeviceState& state);

    /// Never returns nullptr; before the first publish this is a default DeviceState with
    /// everything offline and invalid, which is the truth at that moment.
    StateHandle snapshot() const;

private:
    mutable std::mutex m_;
    StateHandle        current_;
};

/// Owns the stores for every known device.
///
/// The MVP allows one active device, enforced by DeviceManager rather than by the interfaces:
/// nothing here is a singleton, so multiple RS485 devices later need no redesign.
class DeviceManager {
public:
    static constexpr size_t kMaxActiveDevices = 1;

    /// Returns nullptr when kMaxActiveDevices is reached. Re-adding an existing id returns
    /// the existing store.
    StateStore* add(const DeviceId& id);

    std::vector<DeviceId> devices() const;
    StateHandle           state(const DeviceId& id) const;
    StateStore*           store(const DeviceId& id);
    bool                  contains(const DeviceId& id) const;
    size_t                size() const;
    void                  clear();

private:
    struct Entry {
        DeviceId                    id;
        std::unique_ptr<StateStore> store;
    };
    mutable std::mutex m_;
    std::vector<Entry> entries_;
};

}  // namespace heliograph
