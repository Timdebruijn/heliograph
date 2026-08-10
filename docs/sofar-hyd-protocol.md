# Sofar Solar HYD 3…6K-EP — Modbus RTU

**Experimental**, like every register map here — but better sourced than most. Two independent
sources agree on **sixteen of the twenty mapped registers**: address, width, signedness and scale.
One of them is a published configuration running against a real HYD-3600-EP. The four exceptions —
the two energy counters, the grid figure and house load — are marked *vendor only* below.

That is not the same as this bridge having talked to one. Nobody on this project owns a Sofar.

## Which models

The vendor document covers **HYD 5…20K-3PH** (three-phase) and **HYD3…6K-EP** (single-phase).
This profile is the **single-phase EP** only.

The three-phase siblings now have their own profile, **`sofar_hyd_3ph`**. It shares this map and
adds phases S and T — and those two phases, the thing that makes it three-phase, are the part
**no second source could confirm**: the hardware behind source B is a single-phase HYD-3600-EP,
which has no S or T phase to check against. Everything the two profiles share carries the
sourcing described here; what is new in the three-phase profile is vendor-document only.

One trap worth naming for anyone extending it: **R, S and T are not evenly spaced.** They sit at
0x048D, 0x0498 and 0x04A3, eleven registers apart, with other grid values in between. Assuming a
stride is the obvious way to read the wrong register and get a plausible number back.

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
| `0x0488` | `grid.power` | s16 | **×−10** → W | **vendor only** |
| `0x0504` | `load.power` | s16 | ×10 → W | **vendor only** |

**Battery sign is stated by the vendor and already points the right way**: charge positive,
discharge negative — which is what `battery.power` means. No sign work was needed.

**The temperature row is why a second source earns its keep.** The vendor list offers three
plausible candidates — `Temperature_Env1` (0x0418), `Temperature_HeatSink1` (0x041A) and
`Temperature_Inv1` (0x0420) — and says nothing about which one an operator should read. The second
source, running on hardware, publishes 0x0420. That is a choice made by something that could see
the readings.

**Watch the two energy scales**: today is 0.01 kWh and the lifetime total is 0.1 kWh. They differ
by a factor of ten in the vendor list, and they are adjacent.

## The grid figure, and a mistake worth keeping

`0x0488 ActivePower_PCC_Total` is mapped to `grid.power` with a **negative scale**. The vendor
reports positive when power is *fed into* the grid; `grid.power` is positive when *importing*. A
`scale` of −10 does both jobs: 0.01 kW to watts, and the sign flip.

**The first version of this profile left it unmapped**, on the stated grounds that the vendor gave
no sign convention. That was wrong. The remark cell **wraps onto a second line** in the vendor
table, and the extraction being read showed only the first half — *"Totall [sic] PCC active
power,"*. The continuation reads *"positive to fed [sic] into the grid, negative to draw from the
grid"*. That text was visible on the neighbouring apparent-power row, and it was concluded to
belong only there.

It is kept here rather than quietly corrected because the failure was not misreading a number. It
was **treating a truncated extraction as if it were the document** — and the same class of error
would silently swallow any wrapped cell in any vendor PDF.

`0x0504 ActivePower_Load_Total` came out of the same re-read. The vendor states its direction
plainly — *"Consumed by load is positive"* — which is already what `load.power` means, so it needs
no sign work.

## What is not mapped, and why

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

1. **`0x0488`'s sign in practice** — export should read negative. The vendor states it, but this
   is the reading an installer can check in one glance and the one that matters most.
2. **`0x0485`'s direction**, against the inverter's display while producing.
3. **The four vendor-only rows**: both energy registers, grid power and house load.
4. Whether the same map holds for the **three-phase** siblings.

Four answers and this map moves from `experimental` to `beta`.
