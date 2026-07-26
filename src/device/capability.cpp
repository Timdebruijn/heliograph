// SPDX-License-Identifier: MIT

#include "capability.h"

namespace heliograph {

const char* capabilityName(InverterCapability capability) {
    switch (capability) {
        case InverterCapability::ReadAcPower:              return "read_ac_power";
        case InverterCapability::ReadAcVoltage:            return "read_ac_voltage";
        case InverterCapability::ReadAcCurrent:            return "read_ac_current";
        case InverterCapability::ReadGridFrequency:        return "read_grid_frequency";
        case InverterCapability::ReadDcPower:              return "read_dc_power";
        case InverterCapability::ReadDcVoltage:            return "read_dc_voltage";
        case InverterCapability::ReadDcCurrent:            return "read_dc_current";
        case InverterCapability::ReadEnergyToday:          return "read_energy_today";
        case InverterCapability::ReadEnergyTotal:          return "read_energy_total";
        case InverterCapability::ReadTemperature:          return "read_temperature";
        case InverterCapability::ReadOperatingHours:       return "read_operating_hours";
        case InverterCapability::ReadStatus:               return "read_status";
        case InverterCapability::ReadErrors:               return "read_errors";
        case InverterCapability::ReadMultiplePhases:       return "read_multiple_phases";
        case InverterCapability::ReadMultipleMppts:        return "read_multiple_mppts";
        case InverterCapability::ReadBatteryState:         return "read_battery_state";
        case InverterCapability::SetActivePowerLimit:      return "set_active_power_limit";
        case InverterCapability::SetExportLimit:           return "set_export_limit";
        case InverterCapability::StartStop:                return "start_stop";
        case InverterCapability::SetReactivePower:         return "set_reactive_power";
        case InverterCapability::SetBatteryChargeLimit:    return "set_battery_charge_limit";
        case InverterCapability::SetBatteryDischargeLimit: return "set_battery_discharge_limit";
        case InverterCapability::SetBatteryOperatingMode:  return "set_battery_operating_mode";
        case InverterCapability::SetMinimumSoc:            return "set_minimum_soc";
        case InverterCapability::SetMaximumSoc:            return "set_maximum_soc";
        case InverterCapability::SynchronizeTime:          return "synchronize_time";
        case InverterCapability::_Count:                   break;
    }
    return "unknown";
}

}  // namespace heliograph
