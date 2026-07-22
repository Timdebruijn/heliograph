// SPDX-License-Identifier: MIT

#include "command.h"

namespace heliograph {

const char* commandResultName(CommandResult result) {
    switch (result) {
        case CommandResult::Ok:           return "ok";
        case CommandResult::Unsupported:  return "unsupported";
        case CommandResult::Rejected:     return "rejected";
        case CommandResult::ReadOnlyMode: return "read_only_mode";
        case CommandResult::OutOfRange:   return "out_of_range";
        case CommandResult::RateLimited:  return "rate_limited";
        case CommandResult::DriverError:  return "driver_error";
        case CommandResult::Timeout:      return "timeout";
    }
    return "unknown";
}

const char* commandTypeName(InverterCommandType type) {
    switch (type) {
        case InverterCommandType::SetActivePowerLimitPercent:    return "set_active_power_limit_percent";
        case InverterCommandType::SetActivePowerLimitWatts:      return "set_active_power_limit_watts";
        case InverterCommandType::SetExportLimitWatts:           return "set_export_limit_watts";
        case InverterCommandType::Start:                         return "start";
        case InverterCommandType::Stop:                          return "stop";
        case InverterCommandType::SetReactivePower:              return "set_reactive_power";
        case InverterCommandType::SetBatteryChargeLimitWatts:    return "set_battery_charge_limit_watts";
        case InverterCommandType::SetBatteryDischargeLimitWatts: return "set_battery_discharge_limit_watts";
        case InverterCommandType::SetBatteryOperatingMode:       return "set_battery_operating_mode";
        case InverterCommandType::SetMinimumSoc:                 return "set_minimum_soc";
        case InverterCommandType::SetMaximumSoc:                 return "set_maximum_soc";
        case InverterCommandType::SynchronizeTime:               return "synchronize_time";
        case InverterCommandType::_Count:                        break;
    }
    return "unknown";
}

InverterCapability requiredCapability(InverterCommandType type) {
    switch (type) {
        case InverterCommandType::SetActivePowerLimitPercent:
        case InverterCommandType::SetActivePowerLimitWatts:
            return InverterCapability::SetActivePowerLimit;
        case InverterCommandType::SetExportLimitWatts:
            return InverterCapability::SetExportLimit;
        case InverterCommandType::Start:
        case InverterCommandType::Stop:
            return InverterCapability::StartStop;
        case InverterCommandType::SetReactivePower:
            return InverterCapability::SetReactivePower;
        case InverterCommandType::SetBatteryChargeLimitWatts:
            return InverterCapability::SetBatteryChargeLimit;
        case InverterCommandType::SetBatteryDischargeLimitWatts:
            return InverterCapability::SetBatteryDischargeLimit;
        case InverterCommandType::SetBatteryOperatingMode:
            return InverterCapability::SetBatteryOperatingMode;
        case InverterCommandType::SetMinimumSoc:
            return InverterCapability::SetMinimumSoc;
        case InverterCommandType::SetMaximumSoc:
            return InverterCapability::SetMaximumSoc;
        case InverterCommandType::SynchronizeTime:
            return InverterCapability::SynchronizeTime;
        case InverterCommandType::_Count:
            break;
    }
    return InverterCapability::_Count;
}

}  // namespace heliograph
