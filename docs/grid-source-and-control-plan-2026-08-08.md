# Grid source and local control — design note, 2026-08-08

**Nothing here is built.** This is a design, agreed section by section, written down so the
reasoning does not have to be derived a second time. Read it as intent, not as description.

**Most of it is deliberately deferred.** The agreed priority is:

1. **Primary** — connect to inverters and let Home Assistant steer them.
2. **Secondary** — read other Heliographs and batteries, and eventually decide locally.
3. **Throughout** — setup must stay easy for an end user, the way SolarAssistant and evcc are.

That order matters more than anything else here: the control layer in sections 6 and 7 is
designed but **not next**. See [Three tracks](#three-tracks).

**Three things in it are unmeasured or unconfirmed**, and only one of them is a number. They are
listed under [Open questions](#open-questions), and each gates a decision rather than following
from one.

## Why

Heliograph should be able to know the household's grid import/export figure. Not because the
bridge must close a control loop — Home Assistant and evcc do that well — but because a bridge
that knows it can eventually do so *when HA is down*, and because the figure is worth publishing
on its own.

The canonical model already names the channels. Every output already knows how to publish them.
Nothing fills them.

## What already exists, and what is actually missing

This section is the audit result, and several entries corrected an assumption held at the start.

**The grid channels are vocabulary only.** `grid.import_power` and `grid.export_power` exist as
ids and are in `kAll[]`. No driver and no profile declares them — not even as unsupported. But
the consuming side is complete: the Modbus TCP register map publishes both with validity bits,
Prometheus has metrics for them, the dashboard has tiles, and Home Assistant discovery already
enumerates them for cleanup. **A source that fills these two ids lights up every output with no
output change at all.**

**The write path is complete, and it is not the blocker.** An earlier claim in this project's own
notes — that no shipping driver can write — is wrong for the profile driver. Its `execute()`
resolves the write row, handles numeric and enum commands, re-validates independently of the
dispatcher, and sends the option's own raw value rather than the selection index (a device that
numbers its modes 0, 2, 3, 4 would otherwise be sent 1 for the second option). The whole chain
exists: REST/MQTT to the command queue, drained by the bus task, through four dispatcher gates,
into the driver.

What stands between today and "HA curtails an inverter" is **two gates and no code**:

| Gate | Where | State |
|---|---|---|
| `verified` per write row | build time, in the profile | **0 of 12 rows**, across 7 profiles |
| `modbus.writeEnabled` | runtime | default off |
| `security.readOnlyMode` | runtime | default on |

The `verified` flag moves only after a bench check on real hardware. No design work removes that.

**The state store is medium-agnostic; the driver binding is one level up.** `StateStore` holds a
`DeviceState` behind a mutex and knows nothing about drivers. The binding lives in
`DeviceContext`, which is documented as the only writer of a device's state and runs on the bus
task. Of the driver interface, `DeviceContext` uses exactly four methods: `identity`,
`capabilities`, `busErrors`, `poll`. It never calls `begin` or `probe`.

**The dispatcher cannot see data, by construction.** Its four gates are read-only mode,
capability, range/step, and rate limit, with the kill switch re-read immediately before execute.
It receives a command and a driver — no state, no measurements, no clock beyond rate limiting. A
freshness gate therefore changes what the dispatcher can see; it is not an extra `if`.

**No HTTP client and no TLS are linked.** Symbol counts on the tightest board's current image:
`mbedtls_ssl_` 0, `mbedtls_x509` 0, `esp_tls_` 0. Crypto primitives are present; the TLS record
layer is not. mDNS is linked but used only to advertise. ArduinoJson is linked. The MQTT client
already subscribes and already handles inbound commands.

The bridge deliberately opens no outbound connection: the update check runs in the browser
specifically so that "runs entirely on your own network" stays true.

**Budget on the tightest board**: flash 44.8% used, leaving about 1.8 MB in the app slot; static
RAM 22.3%; minimum free heap measured around 130 KB idle. Flash is not a constraint. Heap is.

## The decisions

### 1. The grid channel is signed, and the rails stay

Meters report a **signed net figure**. The model has two unsigned unidirectional rails and the
profile schema cannot do arithmetic — a limit that already blocked two shipping profiles, both of
which record it in their own comments.

The model already solved this exact problem once, for the battery: a signed `battery.power`
alongside `battery.charge_power` and `battery.discharge_power`, justified as *"the raw rails, for
a device that reads them separately and a consumer that wants them apart"*. Grid is structurally
identical. **A signed channel plus the existing rails is therefore consistency, not novelty**, and
it unblocks the two stuck profiles as a side effect.

Precedence, which the battery channels never had to answer: **the signed channel wins for
control, the rails are informational.** A controller must never arbitrate between two sources of
one quantity. Where only rails exist a consumer may combine them — it is C++, it can do
arithmetic — and the result is marked derived. Disagreement is a fault to report, not to average.

### 2. Network devices are drivers; the medium binding moves down

`Transport` is byte-and-bus shaped on purpose: line settings, input flush, bus lock, byte tap.
HTTP does not fit behind it without making four methods meaningless and pushing HTTP framing into
drivers.

But the driver interface is almost medium-agnostic already. The seam falls where the domain says
it should: **`begin` and `probe` are both bus concerns**, used only by discovery, the registry and
setup. So:

| Layer | Holds | Implemented by |
|---|---|---|
| device (medium-agnostic) | poll, identity, capabilities, busErrors, execute, descriptor | every device, serial and network |
| bus driver, extends it | begin(Transport), probe | the five existing drivers, **unchanged** |

`DeviceContext` widens its member type. The RS485 path is not touched — including the EverSolar
driver, whose sunrise-recovery behaviour took eight sunrises to validate.

**A refinement that matters:** `Transport` is the wrong home for HTTP but the *right* home for a
socket carrying a byte protocol. Modbus TCP is not an exception to this decision, it is its other
half — byte protocols through `Transport`, request/response protocols through the link.

### 3. Freshness gates writes, but a gate alone is not safety

A gate refuses new commands. It does nothing about the state already commanded. If the grid data
dies while a charger sits at 16 A, refusing to send holds it at 16 A.

The firmware already worked this out once: a curtailment written to a holding register **survives
a dead bridge and needs a living one to undo**, whereas a de-energised relay coil drops out with
no firmware involved. So:

- stale data must cause a **fallback to be sent**, not merely a command to be refused;
- a dead bridge cannot be defended against on the register path at all. The relay/DRM path is the
  only one with a passive failsafe, and the wiring choice per line is what decides it.

That distinction belongs in the UI and the documentation, not only here.

**The gate is provenance-based, not severity-based.** A command from Home Assistant is not derived
from our grid figure; blocking it because our meter is quiet would break the mode where something
else closes the loop, exactly when things are already going wrong. So a command may carry a
declared data dependency, and only those are checked. A command marked as internally originated
**must** carry one or be refused — internal commands are precisely the ones derived from our own
data, and that closes the hole where a control layer forgets to declare.

Two mechanisms, answering different questions:

- a **deadline** on the command — how old is this decision? The dispatcher already has a clock, so
  its signature does not change.
- a **liveness check** at dispatch — is the input still alive? Injected the same way the clock
  already is.

A decision that waited twenty seconds behind a bus transaction is stale even if the meter is
healthy; a meter that died one second ago invalidates a decision that is not.

**Two thresholds, not one.** Publish staleness (30 s today) was chosen for inverter polling at a
ten-second interval. A grid figure 25 seconds old is fine to display and dangerous to act on. The
control threshold is per source, derived from the device's own update rate, and never larger than
the publish threshold.

**Two hazards that already exist, before any control layer:**

- The command queue holds **one** request and refuses a second. A safety fallback would be
  rejected because a routine ramp command happens to occupy the slot. It must displace, not queue.
- Rate limiting is a **drop, not a defer** — a refused command is simply lost. The dispatcher
  already solved this for start/stop with a separate track, reasoning that the throttle must never
  be the reason a command cannot be issued. A safety fallback belongs on that same protected
  track.

### 4. Network sources, and where each one lands

The capture side of this comparison is measured; the heap figures for TLS are not.

| Source | Layer | Heap | Failure mode |
|---|---|---|---|
| HomeWizard v1 (HTTP GET) | brand knowledge, in drivers | few KB | poll-shaped, fits the existing staleness machine |
| Shelly Gen2 RPC (HTTP POST) | same | **shares the whole implementation with v1** | same |
| **Modbus TCP** | a profile TOML | no HTTP, no JSON | timeouts as usual |
| Shelly outbound WebSocket | brand knowledge | the async server ships a WebSocket that is not currently compiled in | **a dropped connection is itself the staleness signal** |
| HomeWizard v2 | brand knowledge | **TLS — the only expensive item** | push, same advantage |
| MQTT subscribe | topic shape is user config, not brand knowledge | **none** — already connected and subscribing | **weakest**, see below |

**MQTT is weakest exactly where the freshness gate leans.** Retained messages mean a value hours
old can arrive looking current, and broker liveness says nothing about meter liveness. Usable if
the payload carries its own timestamp; otherwise it is observe-grade.

That is a limit, not a disqualification. Where observing is the whole job — see the battery under
[Open questions](#open-questions) — observe-grade is exactly the right tool, and it beats
rebuilding a protocol something else already speaks.

**Push is safer, not merely tidier.** Polling learns a source is gone after N timeouts — three
seconds at a one-second interval, half a minute at ten. A dropped socket is immediate. The price
is that inbound data then lands on the async network task, which is deliberately outside watchdog
coverage; that task becoming a writer of device state is a new role and should be written down
rather than discovered.

**Modbus TCP is the largest unlock, and the schema was already waiting for it.** The profile
schema's transport field already accepts `tcp`, noting that the bridge has no Modbus TCP client
yet. Our own client frames Modbus RTU with a CRC; Modbus TCP uses an MBAP header and no CRC, so
the client cannot simply be pointed at a socket. Two routes, and the choice is strategic:

- use the linked library's TCP client — works, but stands **outside** the profile pipeline;
- **put MBAP framing under our own client** — then every Modbus TCP device becomes a TOML file,
  which is the class containing SolarEdge, Fronius, SMA and Huawei.

The second is chosen. MBAP is pure byte manipulation and fully host-testable, like the RTU codec
beside it. Two caveats: line settings are meaningless for a socket and need an explicit decision,
and the bus discovery baud sweep must not apply.

**Update rate sets the floor for everything downstream.** A P1 meter on DSMR 5.0 reports every
second; an older meter every ten. With a ten-second meter no control loop can be finer than ten
seconds, and that is a property of the meter, not of this design.

### 5. A second table-driven driver, not a second pipeline

The profile schema already declares its own scope: a genuinely different register-map protocol
family *"would get its own table-driven driver and reuse this same profile pipeline"*. So HTTP
JSON sources are a second `driver` value, not a parallel system.

The boundary is already written too, and it decides every case without new criteria:

| Case | Session state? | Lands |
|---|---|---|
| HomeWizard v1 | none | data file |
| Shelly RPC | none | data file |
| HomeWizard v2 token pairing | yes | C++ codec |
| Shelly push | yes | C++ codec |

JSON is not a register map, and that is where a config format grows without limit. The bound is
hard and checkable: **a flat dotted path with at most one numeric index.** No wildcards, no
filters, no conditions, no arithmetic. Anything beyond it is C++.

One field a register map never needed: the device's **expected update rate**, which is where the
control threshold in section 3 comes from.

**A data file still needs a firmware release** — the codegen is build time. The gain is the
contribution barrier and reviewability, not deployment speed.

Rejected: runtime user-defined sources as **control input**. Brand knowledge would move into user
config, there would be no build-time validation, and a mistyped path that silently returns the
wrong field would feed a loop that moves hardware. The escape hatch, if it is ever wanted: a
runtime-defined source may fill a dashboard channel but is not eligible as control input unless
explicitly promoted.

### 6. Network discovery stays separate from bus discovery

They are opposite on every axis. Bus discovery transmits, re-registers devices, sweeps baud rates,
must own the bus exclusively, and is **ambiguous** — which is why a candidate carries a confidence
score and evidence for a human to weigh. mDNS touches nothing, changes nothing, runs alongside
polling, and is unambiguous about identity.

Merging them goes wrong in all three directions: network discovery inherits exclusivity it does
not need, invents a confidence score that is always full, or the bus rules get loosened to fit —
which is the one outcome ruled out from the start.

So: **separate endpoints and separate report storage, one Discovery tab.** The user has one place
to find devices; each route keeps its own gates and refusal rules. Shared storage would make the
concurrency that network discovery is allowed to have into a bug.

mDNS names a candidate; **one harmless fetch confirms it is the device we want** — a Shelly might
be a dimmer. That step is the network analogue of a bus probe, and unlike a bus probe it costs
nothing and changes nothing.

**The candidate list is untrusted input.** mDNS is unauthenticated multicast; a candidate is an
address someone else supplied. Nothing is stored or polled before the user adopts it, and an
adopted device does not automatically become control input. That is the same adoption shape bus
discovery already has.

Where it runs: not on the bus task, and not inside an async network callback — that lesson is
already recorded. The query blocks for its window, and that timeout should be pinned explicitly
rather than inherited from a library default.

**Make the separation a build failure, not an agreement**: a layering rule that the network
discovery module never includes the transport or the bus discovery engine. A rule that is not
grepped rots.

### 7. Modes, and who is in charge

**The data path is not on this ladder.** Grid publishing happens whenever a source is configured.
Turning automation off must not blank a dashboard. The ladder switches the *allocator*.

| Mode | The allocator | Write path |
|---|---|---|
| off | nothing | no internal commands, ever |
| observe | computes the picture: surplus, who is absorbing it, what remains | same |
| advise | additionally computes what it *would* do, and publishes it as data | same |
| act | submits commands, each carrying a declared data dependency | through every gate |

`observe` earns its rung: "surplus is 2.1 kW, the battery is taking 1.4 kW of it, 700 W is
exporting" is worth having with no automation at all.

**One rung comes free.** `advise` plus the existing manual command path *is* the confirm-each-
action mode. And `advise` improves the HA-driven mode rather than competing with it: the published
intent is a legitimate input for HA to act on, which works today with no verified write path.

**The mode is not the kill switch.** The order, which slots into one that already exists rather
than inventing a second:

```
1. read-only mode          refuses everything, whatever the mode says
2. feature enabled         relays / modbus writes
3. mode per actuator       off / observe / advise / act
4. manual override         per actuator
5. the computed intent
```

The freshness gate crosses all of it and **can veto but never authorise**.

**Modes are per actuator, and today there is exactly one that works.** The relay/DRM path is
hardware-verified. Inverter setpoints are plumbed and dormant. An EV charger does not exist as a
concept in this firmware at all. That is a scope fact, not a gap to paper over.

**Override**: it beats the allocator; its duration is chosen when it is set, defaulting to "until
cleared"; and it is **prominently visible**, because the real problem with a forgotten override is
invisibility, not duration. An expiring override returns *control*, not a value — the allocator
recomputes from current data. It survives a reboot, because the alternative is automation quietly
taking a deliberately pinned device back after a power cut. On boot the mode and override are
restored but **acting stays blocked until the device's actual state has been read once** — a
control that is written must also be readable, or nobody can tell what the device is doing, only
what it was last asked to do. If the read-back disagrees with the override, the override is
re-applied, visibly.

**An active override survives the stale-data fallback.** This was argued and settled: the fallback
exists to undo what the *automation* did, because that decision depended on the data. A manual
setpoint did not. Reverting it would substitute our judgement for the user's at the moment we know
*less* than before, not more. The exception is per actuator: where a setting exists to satisfy a
grid connection condition, stale data means compliance cannot be shown, and falling back is right.

For DRM the direction inverts — the safe state is the restrictive one, so an automated curtailment
is held rather than released while data is stale.

**Explainability without a log in the state model.** The measurement set is deep-copied twice per
poll, which is why its ids are pointers rather than strings; a growing list of reason strings is
exactly what that decision warns against. So: **one current decision, no history** — the inputs
used and their values, which rule fired, **which limit bound the result**, the intent, a
timestamp. That is structurally what the state store already does: an immutable snapshot swapped
under a mutex.

History goes where history already lives: transitions on the existing event stream, retrospection
in the existing log buffer. No new mechanism.

Per output: a decision object over REST and MQTT (attributes in Home Assistant, the pattern that
already ships); an enum code for Modbus TCP, because strings do not belong in a register map; and
for Prometheus a bound value plus a small enum — **a reason as a label is a cardinality trap**.

### 8. One allocator, and participants it cannot steer

**Independent per-device rules form a feedback loop through the physical world.** A ramps up, the
grid figure falls, B sees less surplus and backs off, the figure rises, A ramps further. Their
only channel is the measurement, and the measurement has latency — so the oscillation period is
set by the meter, not by anything either rule controls.

The deeper reason is arithmetic: the allocator must reason about the grid figure **net of its own
outstanding allocations**. Give a charger 1400 W two seconds ago and the meter has not caught up,
and that surplus still appears available. "Committed but not yet observed" is the core state, and
it is what makes this not a rules engine.

**Autonomous participants need patience, not bookkeeping.** The grid figure already contains every
load in the house, visible or not — that is why it is the right control input. A self-regulating
battery and a kettle are the same case. What it needs is not to be raced: if the battery ramps
over ten seconds and the allocator grabs the surplus in two, they fight. Observing the battery
turns that arbitration from a guessed settling timer into a measurement, which is a concrete
reason to read it rather than a nice-to-have.

**Priority is an ordered list with a small fixed set of conditions** — a time window and a
state-of-charge threshold, first match wins. Not weights: a score cannot answer "why did the car
get it" in one sentence, and that sentence is what section 7 exists to produce. The decision
snapshot records which condition matched.

**Four stability mechanisms, often conflated, solving different things**: hysteresis against
chatter, a ramp limit against overshoot given measurement latency, a dwell against rapid cycling
and hardware wear, and a floor for actuators that cannot go lower.

The floor is discontinuous and it decides the "enough for one but not both" case. A three-phase
charger does zero or roughly four kilowatts and nothing between. **Allocating 4 kW to something
that needs 4.1 kW wastes all of it**, so the allocator must order by feasibility as well as
priority — and it can, because declared capability bounds already carry minimum, maximum and step,
and the dispatcher already checks them. No negotiation protocol is needed.

Around the floor, hysteresis is asymmetric — start above it, stop below it, with a dwell long
enough that passing clouds do not cycle a contactor.

**Relay dwell is a hardware lifetime requirement, not tuning.** The current rate limit allows a
burst of three and then one per second. Under manual use that is harmless; under a loop chasing
surplus it is an audible contact with a measurably shorter life. And a dwell is not a rate limit:
one bounds frequency, the other forces a minimum time in state. **Dwell applies to optimisation,
never to a safety fallback** — otherwise it delays the one action that must not be delayed.

**DRM steps, it does not calculate.** The DRM layer is four pure functions over roles and relay
patterns, with **no ordering and no watt semantics** — deliberately, since the meaning lives in
the standard and in the inverter. So the allocator escalates one rung, measures, and stops when
export is within limit. It needs a user-confirmed mode ordering, because which lines are wired to
which terminal is an installation choice.

DRM sits at the **bottom** of the ladder. Consumers raise demand and use free energy; DRM lowers
supply and throws it away. Curtail only when the demand side is saturated or unavailable.

**With no grid figure at all**, the two cases differ: never configured means `act` is not a valid
mode — an entry precondition, like the read-back above — while a source that went away triggers
the active fallback.

### 9. More than one Heliograph

Two things already work with no new code.

**A second Heliograph is already a network source.** It serves its status unauthenticated, built
from the canonical model, so the vocabulary needs no mapping at all — a better source than any
vendor JSON.

**And commanding between bridges already works.** Topics are scoped by bridge id, and a bridge
already subscribes to its relay, DRM and command topics. Any publisher that knows the id can
command it — and the receiving bridge's **own dispatcher stays the gate**. Read-only mode,
capability, range and rate limit all still apply locally. Distributing control does not weaken the
safety model, which follows directly from the dispatcher being the single gate every write passes
through, whatever asked for it.

**One bridge is the master, as an explicit config field.** An earlier proposal derived it — the
allocator runs wherever the grid source is — and that is wrong, because the meter may sit on a
satellite's bus. An explicit role is also inspectable, which is the whole theme of section 7.

**Passive is not a new concept**: its own allocator is off, its outputs stay fully on, and its
command path stays open. That is the mode ladder plus the MQTT path that already exists.

Guard rails: exactly one master, detectable by a retained topic that two masters would both see;
zero masters is legitimate but should be visible rather than silent. And the rule is about
**commanding, not reading** — master, HA and evcc may all read the same bridge; they may not all
steer the same device. That applies outside Heliograph too, and belongs in the documentation.

Three things that break and need fixing before this works:

- **Age compounds, and the payload hides it.** A measurement timestamp is milliseconds since *that
  bridge* booted, so it means nothing to another. What is published is a staleness verdict against
  the **publishing** bridge's 30-second threshold — too lax to justify a write. An age, computed
  by the publisher, is the missing field. Stamping arrival at the last hop understates the total.
- **Capabilities are not published**, so a master cannot learn a remote actuator's minimum,
  maximum and step — exactly what it needs to compute feasibility. The fields already exist in the
  model.
- **Re-publishing an ingested measurement doubles Home Assistant**, because uniqueness is scoped
  per bridge. An ingested remote measurement must not be published as if local.

## Three tracks

Not a sequence. Two of these are independent.

**Track A — the integrations. Not blocked, start here.** Fill the grid channels and every output
lights up with no output change; the signed channel unblocks two stuck profiles; MBAP framing
opens a whole device class as data files; mDNS makes them findable. None of it touches the write
path, none needs hardware verification, all of it is host-testable. If nothing else in this note
happens, this is a good release on its own — and it makes the HA-driven mode better, because more
sources means richer entities for whatever is doing the steering.

**Track B — verify one write row. Small, hardware-gated, and it *is* the primary goal.** No code
on the critical path; a bench check and two flags. Independent of track A.

**Track C — local control. Waits for two steerable participants; today there are none.** The
design is written down so it does not have to be re-derived when that changes.

An earlier version of this note recommended an autonomous DRM curtailment loop as the smallest
useful control step, on the grounds that it avoids the unverified write path. That optimises
against the wrong objective once the primary goal is stated: the write path is the thing to fix,
not the thing to route around. It remains a good option for when track C starts.

## Deliberately deferred, with what unblocks each

| Deferred | Unblocked by |
|---|---|
| the allocator and priority rules | a second steerable participant |
| EV charger | a charger driver existing at all |
| inverter setpoints | one profile row verified on hardware |
| HomeWizard v2 and TLS | the TLS heap measured on this board |
| price awareness | prices arriving over MQTT from HA — **no outbound connection** |
| importing to keep charging | prices, since without them it is a blind purchase |
| bridge-to-bridge control | an age field and capabilities in the payload |

## What never belongs in the firmware

The criterion is not capability but failure mode: **the firmware should hold only the logic that
must survive Home Assistant being down.** Logic that needs HA's data cannot survive HA being down,
so putting it in firmware buys nothing and adds a dependency the firmware cannot check. It is also
harder to change there, which works against the requirement that an end user can adjust things.

So: no forecasting or multi-day optimisation, no price scheduling or departure planning, no
cross-domain logic, nothing needing durable history. This is the same reasoning that already puts
the update check in the browser rather than on the device.

## Easy to change later

Policy is runtime; device knowledge is build time. Everything chosen in sections 3, 7 and 8 is
config, and the machinery exists — configuration is patchable over REST and there is a versioned
migration chain that has already been used once for a driver rename.

Where changing later is expensive, be generous now: measurement ids live in MQTT topics, Home
Assistant unique ids and Modbus registers, and moving a published register region is a schema break
for every client reading it. Device ids must never be renamed or stored configuration is orphaned.

Two consequences: **priority rules should be a list of objects with a type field from the start**,
so a third condition is a new value rather than a migration, and **the register region for
decision and control fields should be reserved now** rather than appended later.

Setup must stay easy. Every threshold, dwell and hysteresis figure in this note should have a
default derived from the profile or the actuator type — a knob a user *may* set, never one they
*must*. A required setting is a hard setup.

## Findings worth fixing regardless

These were found while auditing and are real whether or not any of the above happens:

- **The brand list in the layering check has a hole.** It greps for the vendors in `profiles/` but
  not for HomeWizard, Shelly or AEG, so brand knowledge for exactly the newest devices would pass
  silently outside the drivers directory. The rule's own comment records this hole biting once
  before.
- **The command queue's single slot can block a safety fallback** behind a routine command.
- **A rate-limited command is dropped, not deferred**, so a fallback can be lost.
- **There is no relay dwell**, and the current limit permits a cadence that would wear a contact
  under automation.
- **Capabilities are not published over REST**, and **measurement age is not published at all**.

## Open questions

**The AEG SolarCube protocol — partly answered, and not what was assumed.** A community
integration reaches these batteries over **newline-delimited JSON on a raw TCP socket, port
8080** — not Modbus TCP and not HTTP. The "registers 3000–3130" are semantic addresses inside the
JSON payload, and each request carries a command name and the device serial number. By the profile
schema's own boundary that is session state, so it is a **C++ codec**, not a data file. The
boundary held on its first real test without adjustment.

It is **writable** — direction, power setpoint, work mode and state-of-charge limits, with
write-back verification after every command. If it works, the battery becomes a *steerable*
participant, which makes priority between it and a charger enforceable and gives track C its
second participant, over a path independent of the dormant Modbus write path.

**Probing the unit on this network settled two things.** Port **502 is closed** — there is no
Modbus TCP on this device, so the profile pipeline route does not reach it. And port 8080 accepts
a connection and then returns **nothing**: three well-formed requests, a listen-only session, and
both line terminators each produced zero bytes, while rapid reconnections drew a reset.

That silence is not the device being broken. **The community integration reads and controls this
battery successfully today** — the probe was wrong, not the unit. Two explanations remain: either
the serial number filters the request (it was sent as zero), or **the device serves one client at
a time and Home Assistant holds it.** The reset under rapid reconnection points at the second.

**If it is one client at a time, a codec in this firmware would break a working integration in
order to duplicate it.** So the decision is to take the battery from Home Assistant over MQTT
instead: no codec, no socket, no serial number in configuration, and no contention for the only
session. The price is a dependency on Home Assistant, which is acceptable here because this serves
the *secondary* goal — observing the battery and accounting for it in the budget — and not the
primary one.

What is still genuinely open is much smaller than the original question: **which entities that
integration publishes, and whether they are enough for the accounting** described in section 8.

**The TLS heap on this board is unmeasured.** The figure that matters is a single session's cost
against roughly 130 KB minimum free heap. The library defaults suggest tens of kilobytes, but that
is a default, not a measurement, and the HomeWizard v2 decision rests on it.

**Which line does what in the DRM ordering** is an installation fact, not a standard one, and must
be confirmed by the user rather than derived.

## Out of scope

- Any change to the RS485 path or the existing bus discovery rules
- Enabling any dormant write row — that is a bench result, not a decision
- Forecasting, price scheduling, and anything needing durable history
- Runtime user-defined sources as control input
