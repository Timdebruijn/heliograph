# Canonical measurement vocabulary

Every device profile maps registers onto these ids and no others. The vocabulary is what
keeps the outputs (MQTT, Home Assistant discovery, REST, Modbus TCP, Prometheus)
device-agnostic: an output publishes `battery.soc`, never "Growatt register 1014".

The single source of truth is `namespace measurement_id` in
[`src/device/measurement.h`](../../src/device/measurement.h). The build parses that
header directly, so this document can lag but the validator cannot. Print the live list:

```console
$ python3 tools/gen_profiles.py --list-measurements
```

## Ids

| Id | Unit (typical) | Meaning |
|---|---|---|
| `ac.power.total` | W | Total AC active power at the grid terminals |
| `ac.frequency` | Hz | Grid frequency |
| `ac.phase_l1.voltage` | V | Phase L1 voltage |
| `ac.phase_l1.current` | A | Phase L1 current |
| `ac.phase_l1.power` | W | Phase L1 active power |
| `dc.power.total` | W | Total PV (DC) power over all MPPTs |
| `dc.mppt_1.voltage` | V | MPPT/string 1 voltage |
| `dc.mppt_1.current` | A | MPPT/string 1 current |
| `dc.mppt_1.power` | W | MPPT/string 1 power |
| `dc.mppt_2.voltage` | V | MPPT/string 2 voltage |
| `dc.mppt_2.current` | A | MPPT/string 2 current |
| `dc.mppt_2.power` | W | MPPT/string 2 power |
| `energy.today` | kWh | Energy produced today |
| `energy.total` | kWh | Lifetime energy produced |
| `inverter.temperature` | °C | Inverter internal temperature |
| `inverter.operating_hours` | h | Total operating hours |
| `battery.soc` | % | Battery state of charge |
| `battery.power` | W | Battery power — **positive = charging, negative = discharging** (SunSpec convention; see below) |
| `battery.voltage` | V | Battery terminal voltage |
| `battery.current` | A | Battery current |
| `battery.temperature` | °C | Battery temperature |
| `battery.energy_charged` | kWh | Lifetime energy charged into the battery |
| `battery.energy_discharged` | kWh | Lifetime energy discharged from the battery |

## Conventions

- **Battery sign.** `battery.power` is positive while charging, negative while
  discharging — the SunSpec energy-storage convention (Model 120). Many vendors report
  "discharge power" as a positive number; check on hardware which way your device points
  before mapping it, and note the finding in the profile.
- **Named after physics, not vendor registers.** The battery channels are shaped after
  the SunSpec storage model on purpose, so hybrids map onto a standard instead of every
  brand inventing its own vocabulary.
- **Missing is not zero.** A channel your device does not have is simply left out of the
  profile — the outputs then never publish it. Never map a register you are unsure about
  "just in case": an invalid value published as authoritative is worse than an absent
  one.

## Extending the vocabulary

If your device has a channel that genuinely is not expressible (say a second battery, or
three-phase L2/L3 channels), do **not** improvise an id — the build will reject it
anyway. Open an issue or add the constant to `measurement.h` in the same PR, following
the existing naming pattern (`domain.instance.quantity`). New ids should stay
vendor-neutral: name the physical quantity, not the register.
