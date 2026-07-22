// SPDX-License-Identifier: MIT
//
// NVS-backed KeyValueBackend. The only ESP32-specific part of configuration persistence;
// everything worth testing sits behind the interface and runs on the host.

#pragma once

#include "configuration_store.h"

namespace heliograph {

class NvsBackend : public KeyValueBackend {
public:
    bool read(const std::string& key, std::string& value) const override;
    bool write(const std::string& key, const std::string& value) override;
    bool erase() override;
};

}  // namespace heliograph
