# Sofar Solar HYD 3…6K-EP — Modbus RTU

**Experimental**, like every register map here — but better sourced than most. Two independent
sources agree on **every mapped register**: address, width, signedness and scale. One of them is a
published configuration running against a real HYD-3600-EP.

That is not the same as this bridge having talked to one. Nobody on this project owns a Sofar.

## Which models

The vendor document covers **HYD 5…20K-3PH** (three-phase) and **HYD3…6K-EP** (single-phase).
This profile is the **single-phase EP** only.

The three-phase siblings share the register list but populate phases S and T and up to six PV
strings, none of which the second source could confirm. A `sofar_hyd_3ph` profile is a
straightforward addition for someone who can check it against hardware — the addresses are in the
same vendor list.

**Sofar's protocol varies by product family**, and the serial number is what tells you which
family a unit belongs to. A map from a different Sofar family is not a second opinion about this
one; it is a different device. That mattered here — an earlier candidate source turned out to
describe the older HYD ES range at completely different addresses.

## Wiring and line settings

Ordinary RS485: A, B and ground, chained not starred, 120 Ω at each end. See
[rs485-bus.md](rs485-bus.md).

| | |
|---|---|
| Baud | 9600 |
| Format | 8 data bits, no parity, 1 stop bit |
| Function code | 3 (holding registers) |
| Unit id | 1–247, factory default **1** |

## What is mapped

Addresses below are given in hex as the vendor lists them; the profile carries the decimal
equivalents.

| Register | Channel | Type | Scale | Sources |
|---|---|---|---|---|
| `0x0485` | `ac.power.total` | s16 | ×10 → W | both |
| `0x0484` | `ac.frequency` | u16 | 0.01 Hz | both |
| `0x048D` | `ac.phase_l1.voltage` | u16 | 0.1 V | both |
| `0x048E` | `ac.phase_l1.current` | u16 | 0.01 A | both |
| `0x0584`–`0x0586` | `dc.mppt_1.*` | u16 | 0.1 V / 0.01 A / ×10 W | both |
| `0x0587`–`0x0589` | `dc.mppt_2.*` | u16 | same | both |
| `0x0604` | `battery.voltage` | u16 | 0.1 V | both |
| `0x0605` | `battery.current` | s16 | 0.01 A | both |
| `0x0606` | `battery.power` | s16 | ×10 → W | both |
| `0x0607` | `battery.temperature` | s16 | 1 °C | both |
| `0x0608` | `battery.soc` | u16 | 1 % | both |
| `0x0420` | `inverter.temperature` | s16 | 1 °C | both |
| `0x0684` | `energy.today` | u32 | 0.01 kWh | **vendor only** |
| `0x0686` | `energy.total` | u32 | 0.1 kWh | **vendor only** |

**Battery sign is stated by the vendor and already points the right way**: charge positive,
discharge negative — which is what `battery.power` means. No sign work was needed.

**The temperature row is why a second source earns its keep.** The vendor list offers three
plausible candidates — `Temperature_Env1` (0x0418), `Temperature_HeatSink1` (0x041A) and
`Temperature_Inv1` (0x0420) — and says nothing about which one an operator should read. The second
source, running on hardware, publishes 0x0420. That is a choice made by something that could see
the readings.

**Watch the two energy scales**: today is 0.01 kWh and the lifetime total is 0.1 kWh. They differ
by a factor of ten in the vendor list, and they are adjacent.

## What is not mapped, and why

**`0x0488 ActivePower_PCC_Total` — the grid figure — is deliberately absent.**

It is the obvious candidate for `grid.power`, and it is signed. But the vendor's remark for that
row is *"Totall PCC active power,"* and stops there: **no sign convention**. Its neighbour
`0x048A` (apparent power) does state one — *"positive to fed into the grid, negative to draw"* —
but that is a different register, and carrying a convention across from one register to another is
exactly the inference this project refuses. The second source reads the register and states no
direction either.

A grid reading with the sign inverted reports an exporting house as importing the same amount.
That is entirely plausible on a dashboard and exactly wrong, and it would then feed anything that
acts on the figure. One hardware session settles it; until then the channel stays empty.

**The kWh grid counters** (`0x0688`, `0x068C`, `0x0690`) are energy, and the canonical grid
channels are instantaneous power — the same reason the Solis map leaves them out.

**PV3–PV6 and phases S/T** exist in the vendor list because it covers the three-phase models.
Mapping them here would publish zeroes from a device that has no such inputs.

## One discrepancy to be aware of

The vendor row for `0x0485 ActivePower_Output_Total` carries the remark *"Charge is positive,
Discharge is negative"* — battery language on an AC output register. Neighbouring rows repeat the
same sentence, so it reads like boilerplate rather than a statement about this register. Both
sources agree on address, width, signedness and scale; only the **direction** is unconfirmed.
Producing should read positive.

## What a hardware session should settle

1. **`0x0488`'s sign**, which would give this family a grid channel.
2. **`0x0485`'s direction**, against the inverter's display while producing.
3. **The two energy registers**, which only the vendor document carries.
4. Whether the same map holds for the **three-phase** siblings.

Four answers and this map moves from `experimental` to `beta`.
