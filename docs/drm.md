# DRM curtailment — relays as demand-response contacts

Status: **software complete, not yet validated on relay hardware.** Nothing in this
document has touched a real inverter DRM port yet; the relay polarity check and the first
wired test are pending the physical Relay-6CH board.

## What this is

AS/NZS 4777.2 defines **demand response modes (DRM)** for inverters: a set of dry-contact
signal lines on a dedicated port (usually an RJ45 socket labelled DRM/DRED). Closing a
line's circuit asserts a mode — DRM0 demands shutdown, DRM1-4 limit consumption/charge,
DRM5-8 limit generation/discharge. Heliograph's relay boards can drive those lines as
**potential-free contacts**: the bridge becomes the "demand response enabling device"
for an inverter that cannot be curtailed over RS485.

**These are signal contacts. Never switch mains voltage on relays assigned a DRM role.**

## The safety model

Three layers, all of which must agree before a contact moves:

1. `security.read_only_mode` — the global kill switch, on by default. Turn it off in
   *Settings → Security* (or `{"security":{"read_only_mode":false}}` over the API).
2. `relays.enabled` — the feature flag, off by default. A relay board with factory
   settings is inert. *Settings → Relays* warns while this one is open and the first
   is still closed; enabling relays alone actuates nothing.
3. The rate limiter — asserting is throttled; **releasing is never throttled**.

Closing either gate (read-only on, or relays off) releases every relay immediately: a
closed gate can only leave the failsafe state behind. On boot everything starts released.

## Failsafe and the NO/NC wiring rule

Firmware semantics are fixed: **relay energised = DRM line asserted**. The failsafe
decision (made for this project and baked into the firmware) is: *a dead bridge must
leave the inverter running normally.*

That decides which relay terminal to use per line:

| DRM line behaviour at your inverter | Terminal to use |
|---|---|
| Line asserted when circuit **closed** (typical for DRM1-8) | **NO** (normally open) — de-energised = open = not asserted |
| Line asserted when circuit **open** (some DRM0 implementations expect a closed loop to run) | **NC** (normally closed) — de-energised = closed = running |

**Verify against your inverter's manual which convention it uses — implementations
differ per manufacturer, and this document deliberately does not claim a universal
pinout.** A wrong choice inverts the failsafe: a dead bridge would then hold your
inverter down. Check with a multimeter before connecting anything.

## Configuration

Each relay gets a role in *Settings → Relays* (or `relays.roles` over the API):
`none`, or `drm0`..`drm8`. Roles do three things:

- the Home Assistant switch is named after the role ("Relay 1 (DRM0)");
- a **DRM Mode select** appears in Home Assistant with `normal` + every configured role;
- the current mode is derived and reported (`drm_mode` in the status payload,
  `<base>/drm/state` on MQTT). Hand-toggled combinations report as `custom`.

Selecting a mode asserts exactly that role's relay(s) and releases everything else;
`normal` releases all. A mode that fails halfway is rolled back to fully released —
a half-asserted mode is worse than none.

## Control surfaces

| Surface | Path | Notes |
|---|---|---|
| Home Assistant | `select.<bridge>_drm_mode` + per-relay switches | via MQTT discovery |
| MQTT | `<base>/<bridge_id>/drm/set` (mode name), state on `.../drm/state` | QoS 1 |
| REST | `POST /api/v1/drm/set?mode=<name>` | admin-gated |
| Modbus | registers 850/851 | **observation only**, never control |

## Board capabilities

| Board | Relays | Realistic use |
|---|---|---|
| RS485-CAN | none | monitoring only |
| Relay-1CH | 1 | a single line — typically DRM0 |
| Relay-6CH | 6 | multiple lines, e.g. DRM0 + DRM5 + DRM6 |

## Before first use on a real inverter

1. Flash the board, turn off read-only mode, enable relays, and click the switches with
   **nothing connected**:
   you should hear the coils and see the states follow in Home Assistant.
2. Confirm polarity with a multimeter across NO/COM: de-energised must read open.
3. Pull the bridge's power mid-assertion: every contact must return to its de-energised
   state (that is the whole failsafe).
4. Only then wire the DRM port, per the NO/NC table above and your inverter's manual.
