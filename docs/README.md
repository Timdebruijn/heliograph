# Documentation

Thirty-two documents, grouped by **why you are here** rather than by what they are about. Start
in the row that matches what you are trying to do.

New to the project? [The README](../README.md) answers "what is this and does it work with my
inverter" first. Come back here when you need detail.

---

## I want to get it working

| | |
|---|---|
| [../README.md](../README.md) | Which inverters work, what to buy, flash it, join WiFi, wire it, find the inverter. The whole path, start to finish. |
| [hardware.md](hardware.md) | The three supported boards: pins, power, the RTC, the BOOT-button factory reset, and what has and has not been verified on each. |
| [rs485-bus.md](rs485-bus.md) | **Read this before you wire anything.** Three wires, why ground is not optional, chain-not-star, where the 120 Ω terminators go, and giving each inverter its own address. |
| [drm.md](drm.md) | Turning the inverter *down* using the relay boards and its DRM input. Ships off, behind two gates, fails safe. |

## It does not work

| | |
|---|---|
| [rs485-bus.md § When it does not work](rs485-bus.md#when-it-does-not-work) | The three failure modes the bridge distinguishes — timeouts, checksum errors, invalid frames — and what each one points at. Start here: they mean different things and rule out different causes. |
| The Discovery step in [../README.md](../README.md#5-find-your-inverter) | The ordered checklist when nothing answers at all. Swap A/B first; it is the answer more often than anything else. |
| [rest-api.md § Diagnostics](rest-api.md) and [prometheus.md](prometheus.md) | What the bridge itself thinks is wrong — counters, poll durations, reset reasons, stored crash dumps. |

## Does it work with my inverter?

The per-family table in [../README.md](../README.md#which-inverters-work-today) is the short
answer. These are the long ones — sources, register maps, and what each map does **not** publish
and why.

| | |
|---|---|
| [drivers/coverage.md](drivers/coverage.md) | **Generated.** Which channels each profile actually maps, side by side, with each map's status. The fastest way to compare. |
| [eversolar-protocol.md](eversolar-protocol.md) | EverSolar / Zeversolar legacy TL series. The only **Stable** driver. |
| [growatt-sph-protocol.md](growatt-sph-protocol.md) · [growatt-mic-tl-x-protocol.md](growatt-mic-tl-x-protocol.md) | Growatt SPH hybrid; MIC and MIN TL-X string inverters. |
| [deye-sun-xk-sg-protocol.md](deye-sun-xk-sg-protocol.md) · [solis-rhi-protocol.md](solis-rhi-protocol.md) · [sungrow-sh-protocol.md](sungrow-sh-protocol.md) | Single-phase hybrids with a battery port. |
| [huawei-sun2000-protocol.md](huawei-sun2000-protocol.md) · [goodwe-et-protocol.md](goodwe-et-protocol.md) | Huawei SUN2000 (± LUNA2000); GoodWe ET/EH/BT/BH. **GoodWe ships at unit id 247, not 1.** |
| [solax-x1-protocol.md](solax-x1-protocol.md) | SolaX X1. Read this **before buying or wiring** — the first attempt on real hardware returned nothing at all. |
| [solarmax-protocol.md](solarmax-protocol.md) | SolarMax, via the MaxTalk ASCII protocol. The vendor is gone; this is the only way these units report anything. **No SolarMax has ever answered it** — and DC voltage is left unmapped because the sources disagree. |
| [sunspec.md](sunspec.md) | The vendor-neutral standard. One driver for any inverter that implements it. |

> **Every Modbus register map here is `experimental`** — transcribed from documents, never
> confirmed against the inverter it describes. Two agreeing sources are not a device agreeing.
> Check the readings against the inverter's own display before you trust an energy total.

## I want to connect my own tooling

| | |
|---|---|
| [mqtt.md](mqtt.md) | Topics, payloads, Home Assistant auto-discovery, what the bridge subscribes to, and the publish-on-change deadbands. |
| [rest-api.md](rest-api.md) | Every endpoint, its auth, and its payload. Also SSE, backup/restore and firmware upload. |
| [prometheus.md](prometheus.md) | All 52 metrics, which appear only conditionally, and why unknown is absent rather than zero. |
| [modbus-register-map.md](modbus-register-map.md) | The bridge as a Modbus TCP *server* on port 502: register layout, validity bitmap, one unit id per inverter. |

## I want to add my inverter

Most Modbus inverters go in as a data file. No C++.

| | |
|---|---|
| [adding-a-device.md](adding-a-device.md) | **Start here.** How to research a register map, probe the device, identify values, and turn that into a profile. Includes what to do when sources disagree. |
| [device-profiles/schema.md](device-profiles/schema.md) | Every field a profile may contain, and what each one means. |
| [device-profiles/canonical-measurements.md](device-profiles/canonical-measurements.md) | The measurement vocabulary. A profile maps onto these ids and no others. |
| [device-profiles/write-path.md](device-profiles/write-path.md) | What a `[[write]]` row can and cannot express, and the two gates before a byte reaches an inverter. |
| [../profiles/_template.toml](../profiles/_template.toml) | A commented skeleton to copy. |

## I want to understand how it works

| | |
|---|---|
| [architecture.md](architecture.md) | The layering — transport, driver, canonical model, outputs — and why outputs never see a vendor protocol. |
| [decisions.md](decisions.md) | Framework and library choices, with the reasoning and the risks each one carries. |
| [security.md](security.md) | The threat model, what is enforced, and — importantly — [what can actually reach the inverter](security.md#what-can-reach-the-inverter). |

## Records, not instructions

These are **dated snapshots**. They were true when written and are kept as history. Do not follow
them as guidance — a "next step" in one of them may have happened months ago.

| | |
|---|---|
| [audit-2026-07-29.md](audit-2026-07-29.md) | Resource and efficiency audit: tasks, memory, timing, build variants. |
| [esp32s3-hardware-audit.md](esp32s3-hardware-audit.md) | Hardware audit against v0.13.2, 2026-07-26. |
| [solar-assistant-source.md](solar-assistant-source.md) | Notes on a comparable product's MQTT structure. |
| [passive-decode-plan-2026-08-07.md](passive-decode-plan-2026-08-07.md) | **A plan, not a feature.** Turning a passive bus capture into a readable transaction list. Nothing in it is built, and one number it depends on is unmeasured. |
| [grid-source-and-control-plan-2026-08-08.md](grid-source-and-control-plan-2026-08-08.md) | **A design, not a feature.** Reading the household grid figure, and eventually acting on it locally. Nothing in it is built and most of it is deliberately deferred; three things it depends on are still unmeasured or unconfirmed. |
| [solarmax-maxtalk-plan-2026-08-09.md](solarmax-maxtalk-plan-2026-08-09.md) | The desk research behind the SolarMax driver, and which claim came from which source. **Superseded as guidance by** [solarmax-protocol.md](solarmax-protocol.md), which describes what was actually built; this is kept for the reasoning. |

---

## How to keep this accurate

Documentation drifts silently, and this project has been bitten by it: three separate documents
once denied a feature the firmware had, each written when it was true. Two things help.

**Some of this is generated and must not be hand-edited.**
[drivers/coverage.md](drivers/coverage.md) comes from `tools/gen_coverage.py`, and
`tools/check_layering.sh` fails the build if it is stale. The five dashboard screenshots come
from `tools/make_screenshots.py --bridge <ip>`. (`discovery.png` does not — it needs a live bus
and a human clicking, so it is the one image that ages without anyone noticing.)

**The rest is prose, and nothing checks prose.** When you change what the firmware *can do*,
search the docs for the sentence saying it cannot. That is where the errors live.
