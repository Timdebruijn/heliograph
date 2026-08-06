// SPDX-License-Identifier: MIT
//
// Heliograph — firmware entry point.
//
// Boot order matters and is deliberate:
//   1. serial;
//   2. configuration from NVS (nothing else can be decided without it);
//   3. WiFi, or the setup portal when unprovisioned;
//   4. driver + poll task -- started even without a network, because RS485 does not need one;
//   5. outputs, only once there is a network to serve them on.
//
// There are no credentials in this file and none in the image. An unprovisioned device puts
// up Heliograph-Setup-XXXX and waits.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "diagnostics/breadcrumbs.h"
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "app/capture_runner.h"
#include "app/driver_capture_runner.h"
#include "app/device_plan.h"
#include "app/discovery_runner.h"
#include "boards/board.h"
#include "commands/command_dispatcher.h"
#include "commands/command_queue.h"
#include "config/configuration.h"
#include "config/configuration_store.h"
#include "config/nvs_backend.h"
#include "device/device_context.h"
#include "diagnostics/coredump.h"
#include "diagnostics/diagnostics.h"
#include "diagnostics/log_timestamp.h"
#include "diagnostics/logger.h"
#include "ota/ota_manager.h"
#include "drivers/driver_registry.h"
#include "network/rtc_pcf85063.h"
#include "network/time_manager.h"
#include "network/wifi_manager.h"
#include "outputs/modbus_tcp/modbus_tcp_server.h"
#include "outputs/mqtt/mqtt_output.h"
#include "outputs/rest/rest_api.h"
#include "outputs/rest/rest_payloads.h"
#include "relays/drm.h"
#include "relays/relay_controller.h"
#include "status/boot_button.h"
#include "status/status_led.h"
#include "state/state_store.h"
#include "transport/rs485_transport.h"

using namespace heliograph;

namespace {

// Single source of truth for the firmware version. The three numbers feed the Modbus
// diagnostic registers (820-822); the string -- built from the same numbers plus a
// compile-time build stamp -- feeds the REST/MQTT API and the boot banner. Deriving the
// string from the numbers means they can never drift, which they had: bridgeInfo() only ever
// set the string, so the Modbus registers reported the 0.1.0 struct default for every release.
//
// Keep in lockstep with the git tag: the release workflow builds from the tag, so a stale
// value ships a firmware that misreports its own version -- exactly what bit the post-flash
// check on 2026-07-21. 0.9.0 covered the stability + observability + config-transparency
// batch; 0.10.0 added the BOOT-hold factory reset and the Relay-6CH status LED, both verified
// on hardware; 0.10.1 was defect-fixes from the full-codebase review; 0.11.0 added the generic
// SunSpec Modbus driver and the shared Modbus transaction it runs on; 0.12.0 added a second
// vendor register-map profile and made driver options validated rather than free-form; 0.13.0
// polls several inverters on one bus and carries every one of them into every output; 0.13.1
// stops the boot log opening in UTC after a warm reset; 0.13.2 is the cleanup sweep -- every
// warning our own code emitted, the dead symbols, the duplication that had one point of truth
// to move to, and a traceHex() that printed stack bytes when asked to dump zero of them;
// 0.14.0 makes the configuration something you can carry off the board (backup, previewed
// restore, and an undo), records a raw bus for a device no driver can name, and gives the
// settings page the grouping and the spacing it never had; 0.15.0 tells you in the dashboard
// when a newer release exists and installs it in one click -- checked in the browser, never by
// the bridge, and verified against the release checksum before the boot partition flips -- and
// lets the mock driver be a whole simulated fleet instead of one inverter; 0.15.1 changes
// nothing a user can see -- it collapses five duplications onto one point of truth each (the
// config document that was written twice, the JSON size bound that existed three times, the
// rate limiter, the digest-to-hex renderer, the relay safety gates), names the NVS cap that had
// been a bare 3900, and adds two guards so the classes of rot it cleaned up cannot come back:
// a layering rule against comments citing line numbers in our own files, and a test asserting
// the two config documents differ only in their credentials; 0.15.2 makes the admin password
// box on the Logs tab fillable again -- the 5 s refresh re-opened the prompt while it was being
// typed into and blanked the field, so the only way in was to sign in on Settings first (found
// on hardware) -- and stops a late 401 from discarding credentials another request had just had
// accepted; 0.16.0 lets the bridge take a static address instead of only a DHCP lease -- with
// the validation doing the real work, because a wrong address does not fail loudly (the WiFi
// association still succeeds, the bridge simply becomes unreachable), so every mistake visible
// from the settings form is refused before it is stored, including the two whose symptom is an
// absence rather than an error: no DNS while something is configured by name, and no NTP server
// on a network that has no lease to supply one. It also records which MQTT topic tree was
// announced, so a bridge whose base topic or discovery prefix changes can at least say what it
// left behind, and splits Backup and System out of a settings page that had grown to nine
// sections; 0.17.0 makes a bridge with several inverters legible. The per-inverter strip on the
// Dashboard carried watts and nothing else, so the only comparison available was who is
// producing more right now -- it now also shows energy today, AC voltage, temperature and, for
// hybrids, battery state of charge and whether the battery is charging or discharging and by
// how much, said in words rather than as a signed number. Only columns some inverter can
// actually fill appear, because a column of em dashes reads as broken rather than as not
// applicable. The ten-second log heartbeat stops describing the first device as though it were
// the bridge -- it reported one inverter's power and printed a bool under a name that read as a
// count, so four inverters looked exactly like one -- and now gives the fleet, naming every
// inverter that is not answering, why, and how long ago it last replied. Underneath: the two
// PMU drivers stop carrying the same hundred-line transaction loop twice, and AsyncTCP and
// ESPAsyncWebServer move up for two use-after-free fixes on the teardown path the dashboard's
// live updates run over. 0.18.0 lets an operator name their inverters -- "Schuur", "Balkon"
// instead of XH30006011550619 -- everywhere a device is shown, including as the Home Assistant
// device name, while every identifier keys on the id exactly as before, so renaming one keeps
// its history. The Modbus TCP client limit and idle timeout become settings rather than header
// constants nothing ever assigned; six clients came back as four on hardware with nothing
// anywhere saying a limit had been hit, and the bridge now says so. A SunSpec inverter that
// implements model 123 can be curtailed from Home Assistant, which is the first write path in
// this firmware and stays behind the read-only kill switch. And the legacy PMU driver can put
// several inverters on one bus, one device each -- untested against real hardware, since nobody
// involved has two, so the pitfalls are written down rather than claimed away. 0.18.1 repairs
// the per-inverter strip, which 0.18.0 broke the moment a battery appeared in it: the columns
// were allowed a flat width that no longer held once a cell contained words instead of a
// number, so every row wrapped and the table stopped lining up. Columns now declare what they
// need. While it was open, the battery cell also stopped spelling out its direction and started
// showing it -- a down arrow in red for charging, an up arrow in green for discharging, with a
// legend under the table, and the word kept in the cell's title for anyone reading by anything
// other than eye.
// 0.18.2 makes the 32-character limit on a device name mean characters. It counted bytes, so
// an accented name was refused at roughly half the length the error message promised -- in the
// one field that exists to hold a name in somebody's own language. The fix was written during
// the 0.18.0 review and pushed to a branch whose pull request had already been merged, where it
// sat, invisible, through two tags.
// 0.19.0 changes nothing a bridge does. It is the cleanup pass: the diagnostics payload, the
// Modbus transaction skeleton, the REST body guard and the driver lookup each existed twice and
// now exist once, so a rule can no longer be fixed in one copy and left wrong in the other. The
// audit that preceded it found no warnings, no deprecated dependencies and no dead code, which
// is the more useful finding and is why this release is small. What it does add is a guard:
// the release workflow now refuses a tag that disagrees with the version below, because three
// releases in a row that agreed only because someone remembered is not a process.
// 0.19.1 makes the per-inverter strip readable on a phone. With four inverters on the bus it
// showed the device names and nothing else -- every reading sat behind a horizontal scrollbar.
// Below 760px the same table now folds into one block per inverter, every value beside its own
// label; above it the table stays, because four inverters are compared by scanning one column.
// Nothing is hidden either way, which is what separates this from the column-hiding the web
// asset has always refused. CI renders the strip and asserts both shapes.
// 0.20.0 is the release where every output finally reports everything an inverter does. The
// dashboard showed 6 of the 33 channels the model carries and Prometheus exported 8; MQTT, REST
// and Home Assistant discovery were already complete. Both now follow the payload, and a
// canonical id wired into neither fails the build rather than going quietly missing.
//
// MINOR, not patch: Prometheus gained a `phase` label on ac_voltage_volts and
// ac_current_amperes. The names did not change, but a series with a new label is a new series,
// so an existing Grafana panel keeps its history and stops receiving points. See
// docs/prometheus.md.
// 0.21.0 makes a crash dump say WHY. The firmware read the IDF summary and kept three fields
// out of it, discarding the exception cause, the faulting address and a sixteen-deep backtrace.
// The one PC it did report is where the panic HANDLER was running, so it was the one field that
// could not answer the question. The cause and the address need no ELF and no cable:
// "LoadProhibited at 0x00000000" is a null dereference, said in full.
//
// The dump survives reboots in its own flash partition, so this reads crashes already stored,
// not only the next one.
// 0.21.1 stops 0.21.0 naming a fault the dump never recorded. A cause of 0 is EXCCAUSE_ILLEGAL
// on the ISA, but an abort, a failed assert or a watchdog leaves the whole field zeroed -- and
// that is the common case, so "IllegalInstruction at 0x00000000" was an invented fault on a
// dump that was neither. A zero cause is now no cause, and a faulting address is reported only
// for the causes that have one.
//
// It also ships the ELF as a release asset. A backtrace names addresses in the image that was
// RUNNING, and rebuilding a tag afterwards does not reproduce the layout -- which is how the
// dump on the production bridge became undecodable.
// 0.22.0 closes the last structural test gap. The rule that refuses a second instance of a
// driver which cannot share a bus lived inside setup(), the one file the host build does not
// compile -- so a safety property, on the exact bus this bridge exists for, had no test. It is
// app::planDevices() now: a configuration and a registry in, a plan out, and setup() reads the
// verdict instead of computing it.
// 0.22.1 changes nothing a bridge does. Three duplications collapsed to one definition each:
// the Modbus exception frame, the OTA start-of-upload state, and a hand-written search that is
// std::find_if. The audit behind it found no warnings, no deprecated dependencies and no dead
// code, which is the more useful result and why this is a patch.
//
// The exception-frame work carried a real gap out with it: the write path had only the happy
// case, so the check that stops a stale exception from another request being read as the answer
// to this one had never been exercised on the control path.
// 0.23.0 replaces the local web UI. Eight tabs become five -- Live, Inverters, Integrations,
// Health, Bridge -- grouped by what you are doing rather than by which endpoint the numbers came
// from. Live leads with one answer instead of nine equal tiles, and a fleet total always carries
// how many inverters it covers, so "2 037 W" can no longer quietly mean "two of the three
// answered". No endpoint, field or payload changed to do it.
//
// The screen that earns the release is the one for an inverter that has never replied. It used
// to say "check your wiring". It now names the three causes in the order they actually happen --
// two units on one address, A and B swapped, termination in the wrong place -- each with the
// test that settles it beside it, and an honest "no test, check the jumpers" for the one that
// has none.
//
// The session chart is a ring buffer in the browser, filled from the status payload that already
// arrives. The bridge stores no time series and gained no endpoint for it, which is why the
// chart says "held in this browser" on screen.
//
// TWO CHANGES A BRIDGE OWNER WILL NOTICE:
//
// The page is served gzipped and no identity copy is kept, so `curl http://<bridge>/` needs
// --compressed where it did not before. The /api/v1 endpoints are untouched. It buys 139 kB of
// flash -- twice, because there are two OTA slots -- and sends 33 kB instead of 163 kB per load.
//
// One hybrid register map had ac.power.total pointing at an APPARENT power register, so those
// bridges have been publishing volt-amperes as watts -- wrong by the power factor, to MQTT,
// Prometheus, Modbus and Home Assistant alike. It points at real power now, and existing history
// for that series is not comparable across this upgrade. Eleven channels the map ignored came
// with it; four energy registers stay unmapped, because the sources disagree and a plausible
// wrong number is worse than a missing one. Which map, and which registers, is in the release
// notes and in the profile -- naming it here is the drivers layer's business, not this file's.
//
// Also: a driver write path is connected end to end and still gated shut -- no register row is
// marked verified, so nothing can move an inverter yet. And renaming the admin account stopped
// double-encoding a non-ASCII password, which had been signing those users out at the next
// admin action ever since the account could be renamed.
// 0.24.0 is the release that watched somebody unbox one.
//
// A bridge that has just been provisioned has no inverter named in its configuration -- the
// setup portal asks for a network and a password and nothing else -- so the firmware starts its
// highest-priority driver, and that driver answers nothing. From the poll results alone that is
// indistinguishable from a miswired bus, and the page said so: the FIRST screen after setup was
// a red card naming three wiring faults, A/B swapped at the top, about a pair the owner had not
// touched. The Live tab was quieter and no better -- a dash where the production goes and then
// nothing at all. Both now read the configuration, which knows what the poll results cannot,
// and offer the one thing left to do. The diagnosis is unchanged for a bus somebody HAS
// connected, which is the only place it was ever right.
//
// Two things on that screen were simply broken. "Add one by hand" sent a row naming no driver,
// which the firmware refuses by design, so the button could not work at all -- it now picks the
// first free bus address rather than dropping the new unit on top of the existing one. And every
// refresh replaced the tab it was drawing, which shut any open dropdown and emptied any
// half-typed field; with an event arriving up to once a second, the driver list could not be
// read to the bottom before it closed itself.
//
// THE BATTERY CARD CHANGES MEANING, so read this before wondering what happened. One colour used
// to drive both the direction line and the charge bar, which made a full battery draw a red bar
// whenever it happened to be charging. They answer different questions and now say so
// separately: the bar is the LEVEL (red below 20%, amber below 50%, green above) and the line is
// the DIRECTION -- up and green gaining, down and red giving back. Both the colours and the
// arrows are the reverse of 0.18.0's. That arrow used to show which way current flows, which at
// the terminals runs INTO a battery that is charging; it now shows which way the percentage is
// heading, because that is the question somebody reading a percentage is asking.
//
// Also fixed: a "needs restart" badge that was static markup and therefore lit on every bridge,
// always, next to five sibling cards that correctly say "changes here need a restart".
//
// Nothing else a bridge does changes. The rest of this release is a cleanup pass with no
// behaviour in it -- a test that asserted against freed stack and passed by luck, twelve
// duplicated capability declarations with no test on either copy, three tools spelling out the
// same page pipeline, and the config write path stated once instead of at every route that
// writes. A cold native build is warning-free for the first time.
// 0.24.1 is what a week of using 0.24.0 on a real bridge turned up, plus the first driver to
// reach Stable.
//
// THE INVERTERS PAGE STOPS FIGHTING YOU. Every few seconds the page rebuilt the tab it was
// drawing, so an open dropdown shut, a half-typed field emptied, a selected backup file was
// forgotten, the health log scrolled back to the top, and a firmware upload or a raw-bus
// recording had the page redrawn underneath it while it ran. It now redraws only what actually
// changed, leaves any panel you have open alone, and never touches a control you are using.
//
// ADDING A SECOND INVERTER WORKS. "Add one by hand" was handing the new row the same bus address
// the first inverter already answers at -- two units on one address destroy each other's replies
// -- because it read only stored settings and a bridge configured through the wizard stores none,
// answering at its driver's default instead. Address checks now resolve the way the firmware
// does. A row that was added but has not started yet could be deleted but not corrected, so a
// wrong address meant restarting, discovering the mistake, and restarting again; it now opens the
// same form a running inverter has. And a removal the firmware refused used to produce nothing at
// all -- no message, no error, the row still sitting there.
//
// SEVERAL SUNSPEC DEVICES MAY NOW SHARE ONE BUS, each with its own unit id, which is how Modbus
// RTU addresses devices in the first place. The driver refused a second one with no reason
// recorded anywhere; there is no protocol limit behind it. Not confirmed on hardware -- two
// SunSpec devices have never shared a real bus here.
//
// ONE DRIVER REACHES STABLE, the first to do so -- the legacy AA55 one, not named here because
// brand knowledge belongs in src/drivers/ and the rule holds for comments too. The gate was
// unassisted sunrises: the bug it once carried appeared only at the night-to-morning transition,
// and a reboot hides that by starting cold, so no bench test could ever close it. Eight
// consecutive mornings, recovery getting faster rather than slower. The level still does not
// claim two inverters on one bus, and that protocol defines no writes at all.
//
// For anything reading the API: /api/v1/drivers gains supports_multiple_devices, so a client can
// refuse a second row of a one-device-per-bridge driver while somebody is still looking at the
// form, instead of at the reboot afterwards. Devices report config_slot, which says which
// configured row they came from -- the web UI had been guessing that, twice, and both guesses put
// a Remove button on the wrong inverter.
//
// 0.24.2 changes nothing a bridge does. It adds two pieces of observability that came out of
// a resource audit (docs/audit-2026-07-29.md), for whoever reads the diagnostics endpoint --
// nothing in the web UI surfaces either yet.
//
// POLL DURATION: poll_success_total always counted attempts, never how long one took.
// poll_duration_{count,last_ms,min_ms,max_ms,ewma_ms} answer that, successful polls only --
// a failed poll lasts the transaction deadline by construction, and this fleet's inverter is
// dark every night, so counting failures would peg the max at the deadline and drag the
// average toward it a few thousand times per night. Absent, not zero, until the first sample:
// a driver that answers in 0 ms (the mock does) is a real measurement, not a missing one.
//
// RESET BREADCRUMBS: boot_count, previous_uptime_ms and previous_firmware, kept in
// RTC-domain SRAM, which survives a panic, a watchdog reset or a software reset even on a
// board with no battery-backed clock -- the only board this project ships where that
// mattered, because it has no other way to say how long the last life ran or what it was
// running before it died. Clock-free by design: the field is uptime, never wall time, so a
// board that boots at epoch zero is exactly as trustworthy as one with a real RTC. Absent
// on a cold start (first boot, or power was lost -- indistinguishable, and both honestly
// "cold"), because "it had been up 0 ms" would be a false statement, not a missing one.
//
// 0.24.3 closes out the same resource audit: one correction to something 0.24.2 got wrong,
// and one real change to what a bridge does under memory pressure.
//
// MQTT PUBLISHES ARE NOW REFUSED, AND COUNTED, WHEN INTERNAL MEMORY IS LOW -- ON EVERY BOARD.
// The MQTT library already refuses a publish below a memory floor, but the figure it checks
// is the larger of internal heap and PSRAM, and a publish is always small enough to come from
// internal heap regardless. On the two boards with 8 MB of mostly-idle PSRAM that check
// compared kilobytes against megabytes and never fired; the one board with no PSRAM was the
// only one it ever protected. A guard in this firmware now checks the pool a publish actually
// draws from, on all three boards alike, and a refusal counts on mqtt_publish_failure_total
// the same as any other. Nothing changes under normal operation -- this only acts under
// genuine memory pressure, which is exactly when acting matters.
//
// CORRECTED: the previous release's own comment blamed "something in the platform" for reset
// breadcrumbs not surviving a restart in an earlier design. It was not the platform. A default
// value on a struct member (`= 0`) made that type require a constructor the compiler runs on
// every boot, which is precisely what RTC-preserved memory exists to avoid -- proven both
// directions on hardware, one character apart. The struct shipped in 0.24.2 was already
// written without that mistake, so nothing about what a bridge reports changes here; a
// compile-time check now exists so the mistake cannot return unnoticed.
// 0.25.0 makes a register map a data file. Six new inverter families answer this bridge, and
// not one of them needed a line of C++.
//
// SIX NEW FAMILIES, five of them from makers this bridge had never spoken to. They are not
// named here -- brand knowledge belongs in src/drivers/ and the rule holds for comments too;
// docs/drivers/coverage.md is the list. Every register traces to a named source: a vendor
// protocol document, or two mature open-source implementations that agree. Where the sources
// disagreed the row was dropped and the conflict written down rather than settled by picking a
// favourite, so several channels are deliberately absent. A battery power whose direction
// nobody states is worse mapped than missing.
//
// THE DRIVER LOST ITS BRAND. One driver now serves seven manufacturers and cannot honestly be
// named after one of them, so it is `modbus_profile`. Stored configurations migrate themselves
// on first boot, including every extra device on a shared bus, and a restored backup takes the
// same path. Nobody has to retype anything.
//
// FOUR SCHEMA FEATURES, EACH FORCED BY ONE REAL REGISTER, none added because it seemed useful:
// a scaling `offset`, for a vendor that stores temperature biased so it never goes negative on
// the wire; a negative `scale`, for a vendor that states the opposite battery sign convention
// to ours; `word_order`, for families that put the low half of a 32-bit value at the lower
// address; and `invalid` sentinels, for devices that answer an unreadable channel with 0xFFFF
// rather than an error -- without which an inverter asleep at night reports 3276.7 degrees.
//
// EVERY MAP SAYS HOW FAR IT HAS BEEN PROVEN, and says it for itself. A profile carries its own
// status, shown at the moment somebody picks it, because one driver reads all eight tables and
// a driver-level badge can only ever describe the least-proven map in the build. Left that way,
// the first map confirmed on hardware would have promoted every unconfirmed map with it.
//
// ALL EIGHT MAPS ARE EXPERIMENTAL, and that is not a formality. Each is transcribed from
// documents; none has been confirmed against the device it describes. The one defect found in
// review proves why the distinction matters: six rows of one map decoded with their halves swapped
// -- a factor of 65536, four kilowatts of sun reported as 262 megawatts -- sitting behind two
// sources that agreed with each other and a test written from the map it was meant to check.
// Agreement between readers is not confirmation from a device. Check the readings against the
// inverter's own display before trusting an energy total, and report back either way: a map
// that turned out wrong is as useful to the next person as one that worked.
//
// NOTHING HERE CAN OPERATE AN INVERTER. Every setpoint these profiles record stays dormant --
// unverified by construction, and two of them need a Modbus function this firmware does not
// implement. A wrong map can report a wrong number; it cannot put a device into an untested
// state.
#define HELIOGRAPH_VERSION_MAJOR 0
#define HELIOGRAPH_VERSION_MINOR 26
//
// 0.26.0 can record a conversation it is having, not just one it is overhearing.
//
// The raw bus capture has always been PASSIVE by design: the bus task runs it instead of a poll,
// so the bridge is silent for the whole window. Right for an unidentified device, where our own
// traffic would present two conversations as one stream -- and structurally unable to record a
// protocol we already speak, because the driver that would produce that traffic is the thing it
// pauses. Thirty seconds against a live inverter with a working driver: zero frames, zero bytes.
//
// mode=driver arms a recorder instead of taking the bus. Polling continues; that traffic IS the
// recording. One observation made it possible: every driver reaches the bus through Transport,
// and both protocol layers hand a complete request to a single write(), so the transport knows
// which direction each byte went without any driver being asked. No driver changed, and every
// driver added later is covered the day it arrives.
//
// That also makes the framing exact. The passive mode cuts on the t3.5 idle gap, which is honest
// at 9600 and 19200 and approximate above, because at 38400 the real gap is finer than any
// reader resolves. Here the primary boundary is the direction turning around, which holds at any
// baud rate -- and every record says which rule cut it, because a boundary the protocol made and
// one the recorder imposed are otherwise indistinguishable.
//
// What it buys, beyond bytes: a shipped driver can be checked against its protocol document on
// real hardware without a USB-RS485 tap at the inverter, and "requests with no replies" becomes
// a diagnosis the passive mode structurally cannot reach -- there the bridge is the silent one,
// so a dead bus and an unanswering device produce the same empty report.
//
// Cost in the RS485 hot path when nothing is recording: one inline null check. It never takes
// the bus lock and never touches the line.
//
// 0.26.1 MAKES THAT REPORT REACHABLE. 0.26.0 could record a driver capture and could not hand it
// back: the endpoint answered with the PASSIVE report every time -- status "idle", zero frames,
// no sent/received at all -- while the bridge log said "6 frames (3 sent, 3 received), 198
// bytes" in the same minute.
//
// A bare-string URI in ESPAsyncWebServer matches ^uri(/.*)?$, so /api/v1/capture answered
// everything beneath it and, being registered first, won. GET /api/v1/capture/anything returned
// the same document.
//
// The URL is unchanged; the MATCHER is. /api/v1/capture is AsyncURIMatcher::exact now, so it
// answers one URL and stops claiming a subtree it never owned. That over-matching was a defect
// in its own right -- /api/v1/capture/nonsense answering 200 with a report is wrong whatever
// lives beneath it -- and renaming the child would have left it in place.
//
// This file already carried the answer. The note above /api/v1/devices tells the same story
// about a different route, reaches for the same matcher, and ends "It did exactly that, and the
// web UI crashed on the wrong shape rather than getting a clean 404." The capture route was
// added as a bare string three hundred lines below that paragraph.
//
// WHAT FOUND IT: flashing it. Not the host tests, not CI, not the review passes -- all of which
// ran over this code and none of which could reach it, because the routing lives in the part of
// rest_api.cpp inside `#if defined(ESP32)` and the host build compiles a stub. (Counts left out
// deliberately: they would be wrong within a month and would make a still-true paragraph read
// as stale.) check_layering.sh rule 11 covers the part of this a machine can check.
// 0.26.2 changed NO behaviour. It existed so a binary had a name: main had moved sixteen source
// files past v0.26.1 while still declaring 0.26.1, and SerialProfile lost a byte in the process,
// so two different images both called themselves 0.26.1. That mattered because validating a
// board against an untagged build says nothing about which build was validated.
//
// The reason given at the time was that "the Relay-1CH has never booted 0.26.x". THAT WAS
// FALSE, and it is worth leaving here rather than deleting. The board answers the question in
// one GET -- it reports previous_firmware 0.26.2 with previous_uptime_ms 1542767, about
// twenty-six minutes -- and nobody asked it. The claim came out of a notes file, was repeated
// through a working session, and was carried into two tag messages, and every one of those
// repetitions was cheaper than the check would have been. The argument for tagging held
// anyway; the fact supporting it did not.
//
// 0.26.3 DOES change behaviour, which is the difference from the release before it. Home
// Assistant now hears about twelve diagnostic entities it was never told existed: the payload
// has carried between thirty-five and forty-two fields for a long time and discovery announced
// seven, so poll duration, stack headroom, PSRAM and the quiet failure counters were on the bus
// and invisible in the place anyone looks. A stored coredump became a binary_sensor, and the
// remainder rides along as attributes on one entity rather than as thirty more recorder streams.
//
// Also fixed here: wifi_rssi_dbm is null while unassociated, and its entity had shipped with an
// unguarded template since the day it was written, so Home Assistant would store the string
// "None" as a signal strength.
//
// NOT PROVEN BY ANYTHING IN THIS REPOSITORY: that Home Assistant renders these templates the way
// the tests assume. There is no Jinja engine in the suite; the guarded-template behaviour rests
// on Home Assistant's documentation. This release is the first chance to find out on hardware,
// which is the same seam that hid the capture routing bug until 0.26.1 was flashed.
// 0.26.4 is the backup-and-restore round, and it is small on purpose. Two things a person sees:
// the restore preview breaks a changed list into one row per field instead of serialising the
// whole array onto one row, and the message after a restore names the undo control instead of
// pointing at where it is not.
//
// Six commits touched src/ since v0.26.3 and only two of them change behaviour; the other four
// are comment corrections -- verified by diffing each one with comment lines stripped, rather
// than by reading the subjects. Worth doing before writing a release note that says "small".
#define HELIOGRAPH_VERSION_PATCH 4
#define HELIOGRAPH_STRINGIFY_(x) #x
#define HELIOGRAPH_STRINGIFY(x) HELIOGRAPH_STRINGIFY_(x)
constexpr uint16_t kFirmwareMajor = HELIOGRAPH_VERSION_MAJOR;
constexpr uint16_t kFirmwareMinor = HELIOGRAPH_VERSION_MINOR;
constexpr uint16_t kFirmwarePatch = HELIOGRAPH_VERSION_PATCH;
constexpr const char* kFirmwareVersion =
    HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_MAJOR) "." HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_MINOR)
    "." HELIOGRAPH_STRINGIFY(HELIOGRAPH_VERSION_PATCH) " (" __DATE__ " " __TIME__ ")";

Rs485Transport     g_transport;
DriverRegistry     g_registry;
DeviceManager      g_devices;
Diagnostics        g_diagnostics;
NvsBackend         g_nvs;
NvsBackend         g_nvsLegacy{kLegacyStorageNamespace};  // pre-rename config, read-only
ConfigurationStore g_store{g_nvs, &g_nvsLegacy};
Configuration      g_config;
WifiManager        g_wifi;
TimeManager        g_time;
modbus::ModbusTcpServer           g_modbus;
std::unique_ptr<mqtt::MqttOutput> g_mqtt;
std::unique_ptr<rest::RestApi>    g_rest;

/// One entry per configured device, in poll order. Element 0 is the `driver` section; the rest
/// come from `additional_devices`. Held as parallel owning vectors rather than one struct so
/// the existing single-device call sites (g_driver / g_context / g_state below) keep meaning
/// exactly what they meant: the FIRST device.
std::vector<std::unique_ptr<InverterDriver>> g_drivers;
std::vector<std::unique_ptr<DeviceContext>>  g_contexts;

/// The first device, or nullptr. What is left that still speaks about "the" inverter: the
/// boot-confirm check, the REST /status device block, and the "is anything polling at all"
/// guard in the poll loop. Naming them rather than indexing at each call site keeps the
/// remaining single-device assumptions countable -- every use of g_driver/g_context/g_state is
/// a place that has not been taught about the others. The outputs no longer belong on that
/// list: MQTT, REST, Modbus TCP and Prometheus all carry every device.
InverterDriver* g_driver  = nullptr;
DeviceContext*  g_context = nullptr;
StateStore*     g_state   = nullptr;

/// Round-robin cursor for the poll loop, so a device whose backoff has expired does not always
/// lose to the one before it in the list.
size_t g_pollCursor = 0;

/// Device ids, index-aligned with g_contexts, so a log line can name the inverter it means.
std::vector<DeviceId> g_deviceIds;

/// The id of the CONFIGURED first device, if it started. Empty otherwise -- and empty is the
/// point: it is what stops a boot where device 1 failed from handing its MQTT topics, its Home
/// Assistant entities and its recorder history to whichever device did start.
DeviceId g_primaryDeviceId;

/// Whether the announced-device reconciliation is done for this boot. Once per session, and
/// only once the broker is actually connected: publishing the clears into a disconnected client
/// would drop them silently and then record the new list as if they had gone out. A refused
/// publish leaves this false so the next pass retries, bounded by the counter below.
bool     g_announcedReconciled = false;
unsigned g_announcedAttempts   = 0;

/// How many devices the configuration asked for, whether or not they started. Drives the status
/// LED: a configured device that failed to start is a fault to show, not an absence to ignore.
size_t g_devicesConfigured = 0;

/// One line per configured device that is not being polled, in configuration order. Filled at
/// boot alongside the log lines, so the same facts reach a screen instead of only a ring buffer.
std::vector<std::string> g_deviceProblems;

/// The started device at each CONFIGURED position, empty where one did not start.
///
/// This is what the Modbus unit ids are keyed on, and it exists because g_deviceIds is
/// compacted: a device that fails to start is simply absent from it, so keying unit ids on that
/// list would silently move every later inverter down one unit id. A client reading unit 2
/// would get inverter 3's watts with no way to notice, and unit ids are a wire contract nobody
/// re-derives after bring-up. Keyed on the configuration instead, an unstarted device's unit id
/// answers "offline, no data" -- which is the truth, and the same thing the Devices tab shows.
std::vector<DeviceId> g_configSlotIds;

bool g_outputsStarted = false;

/// Guards g_config against the one cross-task hazard it has: the AsyncTCP task replacing
/// the whole object (config PATCH / provision, via ctx.applyConfig) while loop()/rs485Task
/// read its std::string members. Readers on the AsyncTCP task itself need no lock -- they
/// are serialized with the writer by the library's single event task.
std::mutex g_configMutex;

/// When to reboot, or 0. See requestReboot below.
// Atomic, not just volatile: written by the AsyncTCP task, read by the Arduino loop task, and
// a 64-bit load/store is not a single instruction on this 32-bit MCU -- volatile alone allows
// a torn read. Consistent with g_manualPollRequested.
std::atomic<uint64_t> g_rebootAtMs{0};
/// Set by the REST poll action, consumed by rs485Task: the task owns the bus, the web thread
/// only ever asks. See the note at ctx.requestPoll.
std::atomic<bool> g_manualPollRequested{false};

/// esp_timer, NOT millis(): millis() is uint32 and wraps every 49.7 days, and casting the
/// wrapped value to uint64 does not un-wrap it. Every `now < deadline` comparison downstream
/// (MQTT reconnect back-off, WiFi retry schedule, poll due-time, relay rate limiter) would
/// see time jump backwards once per wrap and could stall until the next one — on a device
/// that is up for months, that is a scheduled outage. esp_timer_get_time() is a true 64-bit
/// microsecond counter: monotonic for ~292k years. Same fix in Rs485Transport::nowMs() and
/// the log-timestamp provider.
uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

/// The crash dump the previous boot left behind, read ONCE in setup().
///
/// Not per request: reading it verifies a checksum over the whole stored image. It also cannot
/// change while we run -- a new dump is only written by a panic, and a panic does not come back
/// here -- so a cached copy is not merely an optimisation, it is the accurate model. Cleared in
/// place when the erase action succeeds, which is the one thing that CAN change it.
diag::CoredumpSummary g_coredump;

/// The bridge's DRM relays (empty on boards without them). Commands arrive on two tasks
/// (REST via AsyncTCP, MQTT via the client's task); g_relayMutex serialises them and the
/// state reads in bridgeInfo(). The controller itself stays lock-free and host-testable.
RelayController g_relays{nowMs};
std::mutex      g_relayMutex;

/// The write path's gate and its request slot -- wired ahead of any driver that can accept a
/// command (every shipping driver returns CommandResult::Unsupported). readOnlyMode() follows
/// g_config.security.readOnlyMode on exactly the same two sites as g_relays' does, so the one
/// kill switch covers both. REST and MQTT both submit here (submitCommand below); rs485Task
/// drains it alongside discovery and capture, the same "request only, the bus owner runs it"
/// shape as everywhere else on this bus.
CommandDispatcher     g_commandDispatcher{nowMs};
CommandQueue          g_commandQueue;
std::atomic<uint32_t> g_commandRequestCounter{0};

/// Assigns a request id when the caller supplied none, then enqueues. The ONE function both
/// ctx.submitCommand (REST, AsyncTCP task) and MqttOutput::setCommandHandler (MQTT task) are
/// wired to, so both transports share one counter and one queue instead of two independent
/// paths that could each think they alone hold "the" pending slot.
std::optional<std::string> submitCommand(const std::string& deviceId, InverterCommand command) {
    if (command.requestId.empty()) {
        // kAutoRequestIdPrefix is reserved: parseCommandRequest refuses any caller-supplied id
        // starting with it, so this can never collide with one a REST/MQTT caller picked.
        command.requestId =
            kAutoRequestIdPrefix + std::to_string(g_commandRequestCounter.fetch_add(1) + 1);
    }
    const std::string requestId = command.requestId;
    if (!g_commandQueue.submit({deviceId, std::move(command)})) {
        return std::nullopt;
    }
    return requestId;
}

/// BOOT-hold factory reset and the status LED, on boards that carry them (board::kHasBootButton
/// / kHasStatusLed). Both are sampled from loop() only, so no locking: g_bootPressed and
/// g_statusLedColor are atomics purely so bridgeInfo() (loop + rs485Task) can read them for the
/// REST payload. 5 s hold, long enough that a factory reset is never one accidental brush.
status::HoldDetector        g_bootHold{5000};
std::atomic<bool>           g_bootPressed{false};
std::atomic<status::LedColor> g_statusLedColor{status::LedColor::Off};

/// Owns discovery runs. The web handler requests; rs485Task runs, because it owns the bus.
DiscoveryRunner g_discovery{g_registry, nowMs};

/// Owns passive bus captures, on the same terms and for the same reason.
CaptureRunner g_capture{nowMs};

/// Owns driver captures. Same request-from-the-web, act-on-the-bus-task split, opposite
/// mechanics: this one never takes an iteration away from polling, because the polling is what
/// it records.
DriverCaptureRunner g_driverCapture{nowMs};

/// The configured driver, or the highest-priority one compiled in. No manufacturer name here.
std::string selectedDriverId() {
    if (!g_config.driver.id.empty() && g_registry.contains(g_config.driver.id)) {
        return g_config.driver.id;
    }
    const auto available = g_registry.availableDrivers();
    return available.empty() ? std::string{} : available.front().id;
}

/// Puts the stored line-settings override back on the UART, and says what the line is either
/// way.
///
/// Called after EVERY begin(): at boot, and again after a discovery run, because begin()
/// unconditionally configures the driver's own first profile. Missing the second call meant
/// running discovery from the web UI on a healthy bridge silently reset the line and the
/// inverter went quiet until the next power cycle -- the same failure the override exists to
/// prevent, on the one path that reconfigures the line at runtime (review, 2026-07-25).
/// The line the bus is actually running at.
///
/// Two sources and no third: the stored override when it is on, otherwise the profile the
/// driver's begin() put on the UART -- the first of its recommended profiles, which is exactly
/// what applySerialOverride() below declines to replace. A driver capture reports this because
/// the idle gap derives from it, and because nobody can check the framing against a protocol
/// document a day later without knowing what the line was.
///
/// Caller holds g_configMutex.
SerialProfile effectiveSerialProfile() {
    if (g_config.serial.enabled) {
        return g_config.serial.profile;
    }
    if (g_driver != nullptr && !g_driver->descriptor().recommendedSerialProfiles.empty()) {
        return g_driver->descriptor().recommendedSerialProfiles.front();
    }
    // No driver, or one that declares no profile. Falls back to SerialProfile's own default
    // rather than guessing; with no driver there is no conversation to record anyway, so this
    // is a value for the report to carry, not one anybody acts on.
    return SerialProfile{};
}

void applySerialOverride() {
    if (!g_config.serial.enabled) {
        // Logged even when nothing is overridden. Most bridges follow their driver, and without
        // this line an owner has no way to learn what the bus is actually running at -- which
        // matters the day a firmware update changes a driver's recommendation under them.
        if (g_driver) {
            Serial.println("[serial] line follows the driver's own profile");
        }
        return;
    }
    const auto& p = g_config.serial.profile;
    if (g_transport.configure(p)) {
        Serial.printf("[serial] override: %u baud, %u data bits, %s parity, %u stop\n",
                      static_cast<unsigned>(p.baudRate), static_cast<unsigned>(p.dataBits),
                      parityName(p.parity), static_cast<unsigned>(p.stopBits));
    } else {
        // configure() opens the UART before the direction-pin setup that is the only thing
        // that can fail, so on this path the line IS at the override's speed and framing --
        // what is missing is half-duplex direction control. Saying "the line is still at the
        // driver's setting" would have sent the reader to the wrong hypothesis entirely.
        Serial.printf("[serial] override applied at %u baud, but RS485 direction control "
                      "failed to configure; transmissions may collide\n",
                      static_cast<unsigned>(p.baudRate));
    }
}

// RTC-domain SRAM for the reset breadcrumbs. The type must stay trivially default
// constructible or this object gets zeroed on every boot -- breadcrumbs.h explains why and
// a static_assert there enforces it. Measured, in case someone reaches for one: a `{}` here
// is harmless (constant initialisation, no startup write); it is an initialiser on a MEMBER
// that generates the .init_array entry which defeats the whole point.
RTC_NOINIT_ATTR breadcrumbs::Storage g_breadcrumbStore;
static breadcrumbs::BootRecord       g_bootRecord;

BridgeInfo bridgeInfo() {
    BridgeInfo info;
    info.bootCount        = g_bootRecord.bootCount;
    info.breadcrumbsCold  = g_bootRecord.coldStart;
    info.previousUptimeMs = g_bootRecord.previousUptimeMs;
    info.previousFirmware = g_bootRecord.previousFirmware;
    info.boardName        = board::kName;
    info.boardId          = board::kId;
    info.bridgeId         = g_wifi.bridgeId();
    {
        // bridgeInfo() runs on loop() and rs485Task; the AsyncTCP task can be replacing
        // g_config concurrently (see g_configMutex).
        std::lock_guard<std::mutex> lock(g_configMutex);
        info.name = g_config.bridgeName;
    }
    info.bridgeOnline     = true;
    info.uptimeSeconds    = static_cast<uint32_t>(nowMs() / 1000);  // good for 136 years
    info.freeHeapBytes    = ESP.getFreeHeap();
    info.minFreeHeapBytes = ESP.getMinFreeHeap();
    info.maxAllocHeapBytes = ESP.getMaxAllocHeap();
    // Separate from the three above, which are MALLOC_CAP_INTERNAL. Both accessors are guarded
    // by psramFound() inside the core, so a board without PSRAM reports 0 rather than failing.
    info.psramSizeBytes    = ESP.getPsramSize();
    info.psramFreeBytes    = ESP.getFreePsram();
    info.resetReason      = static_cast<uint16_t>(esp_reset_reason());
    info.wifiConnected    = g_wifi.connected();
    info.wifiRssiDbm      = g_wifi.rssi();
    info.ipAddress        = g_wifi.ipAddress();
    info.staticIp         = g_config.wifi.staticIp();
    info.mqttConnected    = g_mqtt && g_mqtt->connected();
    info.modbusListening  = g_modbus.running();
    info.modbusClients    = g_modbus.activeClients();
    info.firmwareVersion  = kFirmwareVersion;
    info.firmwareMajor    = kFirmwareMajor;
    info.firmwareMinor    = kFirmwareMinor;
    info.firmwarePatch    = kFirmwarePatch;
    info.timeSynced       = g_time.synced();
    info.currentEpoch     = static_cast<int64_t>(time(nullptr));
    info.lastNtpSyncEpoch = static_cast<int64_t>(g_time.lastSyncEpoch());
    const auto ntpSource  = g_time.syncSource();
    info.ntpServer        = ntpSource.server;
    info.ntpFromDhcp      = ntpSource.fromDhcp;
    info.otaImageState    = ota::imageStateName();
    info.coredumpPresent  = g_coredump.present;
    info.coredumpTask     = g_coredump.taskName;
    info.coredumpPc       = g_coredump.programCounter;
    info.coredumpCause        = g_coredump.exceptionCause;
    info.coredumpFaultAddress = g_coredump.faultAddress;
    info.coredumpCauseKnown        = g_coredump.causeKnown;
    info.coredumpFaultAddressKnown = g_coredump.faultAddressKnown;
    info.coredumpBacktrace.assign(g_coredump.backtrace,
                                  g_coredump.backtrace + g_coredump.backtraceDepth);
    info.coredumpBacktraceCorrupted = g_coredump.backtraceCorrupted;
    if (g_relays.count() > 0) {
        {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            info.relayCount    = g_relays.count();
            info.relaysEnabled = g_relays.enabled();
            for (uint8_t i = 0; i < g_relays.count(); ++i) {
                if (g_relays.energised(i)) {
                    info.relayMask |= static_cast<uint8_t>(1u << i);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            info.relayRoles = g_config.relays.roles;
        }
    }
    {
        // From the LIVE configuration, not the boot-time count. A device added on the settings
        // page takes effect at the next restart, so between save and reboot the boot count says
        // "1 configured, 1 polling" and the page cheerfully reports that everything is
        // accounted for -- while the configuration it just stored asks for two. Reading it here
        // makes the same screen say "polling 1 of 2", which is both true and the nudge to
        // restart (review, 2026-07-25).
        std::lock_guard<std::mutex> lock(g_configMutex);
        info.devicesConfigured =
            (g_config.driver.id.empty() ? 0 : 1) + g_config.additionalDevices.size();
    }
    // No lock: written once in setup(), before any task that reads them exists.
    info.devicesStarted = g_deviceIds.size();
    info.deviceProblems = g_deviceProblems;
    info.hasBootButton     = board::kHasBootButton;
    info.bootButtonPressed = g_bootPressed.load();
    info.hasStatusLed      = board::kHasStatusLed;
    if (board::kHasStatusLed) {
        info.statusLedColor = status::colorName(g_statusLedColor.load());
    }
    return info;
}

/// Applies a named DRM mode: the role's relays energised, everything else released.
/// The controller applies the pattern atomically behind its gates, charging ONE rate-limit
/// token for the whole mode switch. Charging per relay (the previous shape) made any role
/// spanning more relays than the burst impossible to assert, ever: the tail ONs always hit
/// the throttle and the rollback released the mode again.
CommandResult applyDrmMode(const std::string& mode) {
    std::vector<std::string> roles;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        roles = g_config.relays.roles;
    }
    roles.resize(g_relays.count(), "none");
    std::vector<bool> pattern;
    if (!drm::patternFor(roles, mode, pattern)) {
        return CommandResult::OutOfRange;
    }
    std::lock_guard<std::mutex> lock(g_relayMutex);
    return g_relays.applyPattern(pattern);
}

std::string scanNetworksJson() {
    const int n = WiFi.scanNetworks();

    // One entry per SSID, strongest BSSID wins. A multi-AP network (UniFi and friends)
    // returns every access point separately, which showed the same name three times in the
    // picker -- pointless, since joining is by SSID and the firmware picks the strongest
    // BSSID itself at connect time (WIFI_CONNECT_AP_BY_SIGNAL). Hidden networks (empty
    // SSID) are skipped: an unnameable entry cannot be chosen from a list anyway.
    struct Network {
        String  ssid;
        int32_t rssi;
        bool    open;
    };
    std::vector<Network> unique;
    for (int i = 0; i < n; ++i) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) {
            continue;
        }
        const auto seen = std::find_if(unique.begin(), unique.end(),
                                       [&ssid](const Network& u) { return u.ssid == ssid; });
        if (seen == unique.end()) {
            unique.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
        } else if (WiFi.RSSI(i) > seen->rssi) {
            // Same SSID on a second AP: keep the stronger one, so the list shows the radio the
            // bridge would actually associate with.
            seen->rssi = WiFi.RSSI(i);
            seen->open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        }
    }

    std::string out   = "{\"networks\":[";
    size_t      count = 0;
    for (const auto& net : unique) {
        if (count >= 20) {
            break;
        }
        if (count++ > 0) {
            out += ',';
        }
        // The SSID lands in JSON. Escape it rather than trust an access point's name -- it is
        // attacker-controlled data by definition.
        std::string escaped;
        for (size_t j = 0; j < net.ssid.length(); ++j) {
            const char c = net.ssid[j];
            if (c == '"' || c == '\\') {
                escaped.push_back('\\');
            }
            if (static_cast<unsigned char>(c) >= 0x20) {
                escaped.push_back(c);
            }
        }
        out += "{\"ssid\":\"" + escaped + "\",\"rssi\":" + std::to_string(net.rssi) +
               ",\"open\":" + (net.open ? "true" : "false") + "}";
    }
    out += "]}";
    WiFi.scanDelete();
    return out;
}

// --- Onboard indicators (BOOT-hold factory reset, status LED, buzzer) ----------------------
// All guarded by the board flags: on a board without them (the RS485-CAN, the 1CH) these are
// dead code the compiler drops, and no pin is touched. Sampled from loop() only.

void initOnboardIndicators() {
    if (board::kHasBootButton) {
        pinMode(board::kBootPin, INPUT_PULLUP);  // pressed reads LOW
    }
    if (board::kHasBuzzer) {
        pinMode(board::kBuzzerPin, OUTPUT);
        digitalWrite(board::kBuzzerPin, LOW);
    }
    if (board::kHasStatusLed) {
        rgbLedWrite(board::kStatusLedPin, 0, 0, 0);  // dark until the first health reading
    }
}

void beep(uint32_t ms) {
    if (!board::kHasBuzzer) {
        return;
    }
    // Active-high, transistor-driven. Blocking is fine: the only caller is the factory-reset
    // path, which reboots immediately afterwards.
    digitalWrite(board::kBuzzerPin, HIGH);
    delay(ms);
    digitalWrite(board::kBuzzerPin, LOW);
}

void driveStatusLed(const status::LedIndication& ind) {
    // Report the logical colour (steady, not the blink phase) so the REST payload reads
    // "red" throughout a factory-reset hold rather than flickering to "off".
    g_statusLedColor = ind.color;

    status::LedColor shown = ind.color;
    if (ind.blink && ((millis() / 300) % 2 == 0)) {
        shown = status::LedColor::Off;
    }
    // Only touch the RMT peripheral when the shown colour actually changes.
    static status::LedColor lastShown = status::LedColor::Off;
    static bool             everWrote = false;
    if (everWrote && shown == lastShown) {
        return;
    }
    everWrote = true;
    lastShown = shown;

    uint8_t r = 0, g = 0, b = 0;
    switch (shown) {
        case status::LedColor::Green: g = 40; break;
        case status::LedColor::Amber: r = 40; g = 18; break;  // warm amber, not yellow-green
        case status::LedColor::Red:   r = 40; break;
        case status::LedColor::Blue:  b = 40; break;
        case status::LedColor::Off:   break;
    }
    // Channel order: this WS2812 lights the RED element from rgbLedWrite's SECOND argument,
    // not the first -- a plain "green" (0,40,0) came out red on the first 6CH hardware run
    // (2026-07-23). So swap red and green here; blue is unaffected. rgbLedWrite's own GRB
    // timing conversion is fine, it is the element mapping on this board that is transposed.
    //
    // rgbLedWrite, not neopixelWrite: the latter is [[deprecated]] in Arduino core 3.x and is
    // now a thin forwarder to this one. Same signature, same behaviour.
    rgbLedWrite(board::kStatusLedPin, g, r, b);
}

/// One call per loop pass: sample BOOT, act on a completed hold, and refresh the LED.
void serviceOnboard() {
    bool holding = false;
    if (board::kHasBootButton) {
        const bool pressed = digitalRead(board::kBootPin) == LOW;
        g_bootPressed      = pressed;
        switch (g_bootHold.update(pressed, nowMs())) {
            case status::HoldDetector::Event::Holding:
                holding = true;
                break;
            case status::HoldDetector::Event::Triggered:
                log::warn("boot: BOOT held; factory reset requested");
                beep(400);  // audible confirmation before the wipe
                g_store.factoryReset();
                Serial.flush();
                ESP.restart();
                return;  // unreachable
            case status::HoldDetector::Event::Idle:
                break;
        }
    }
    if (board::kHasStatusLed) {
        status::LedInputs in;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            in.provisioned   = g_config.provisioned();
            in.mqttEnabled   = g_config.mqtt.enabled;
            in.modbusEnabled = g_config.modbus.enabled;
        }
        in.factoryResetHolding = holding;
        in.wifiConnected       = g_wifi.connected();
        // "A device was configured", not "a device started". The old expression was
        // `g_driver != nullptr`, and the multi-device loop destroys the unique_ptr when begin()
        // fails -- so a bridge whose only configured driver refused to start went from RED to
        // GREEN, reporting "all healthy" while polling nothing (review, 2026-07-25).
        in.inverterExpected    = g_devicesConfigured > 0;
        in.mqttConnected       = g_mqtt && g_mqtt->connected();
        in.modbusListening      = g_modbus.running();
        // Worst-of across every polled device, not the first one. The LED is on the bridge, so
        // it reports the bridge: with three inverters on one bus it showed device 1 and stayed
        // green while the other two were dead -- the same defect as the web header (#38), on
        // the indicator someone standing at the bus is actually looking at. Its three states
        // are kept: red when any device is offline, amber when any is stale or invalid.
        in.inverterOnline = !g_deviceIds.empty();
        in.dataValid      = true;
        in.dataStale      = false;
        for (const auto& id : g_deviceIds) {
            if (StateHandle h = g_devices.state(id)) {
                in.inverterOnline = in.inverterOnline && h->inverterOnline;
                in.dataValid      = in.dataValid && h->dataValid;
                in.dataStale      = in.dataStale || h->dataStale;
            }
        }
        driveStatusLed(status::decide(in));
    }
}

void startOutputs() {
    if (g_outputsStarted || !g_wifi.connected()) {
        return;
    }
    g_outputsStarted = true;

    // Snapshot under the lock, then configure everything from the copy: this runs on the
    // loop task and reads many string members, any of which the AsyncTCP task could be
    // replacing (see g_configMutex). One copy at startup beats fine-grained locking below.
    Configuration configSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        configSnapshot = g_config;
    }

    // Time first: SNTP needs the network (and the DHCP lease that may carry the NTP server), and
    // starting it here means every log line below already gets a wall-clock stamp once it syncs.
    g_time.begin(configSnapshot);

    if (configSnapshot.modbus.enabled) {
        const modbus::ModbusServerConfig cfg = modbus::serverConfigFrom(
            configSnapshot.modbus, static_cast<uint8_t>(g_devicesConfigured));
        g_modbus.setConfig(cfg);
        const bool listening = g_modbus.begin();
        if (g_modbus.servedDevices() <= 1) {
            log::info("modbus: %s on :%u (unit %u)", listening ? "listening" : "failed to start",
                      cfg.port, cfg.inverterUnitId);
        } else {
            log::info("modbus: %s on :%u (units %u-%u, one per inverter)",
                      listening ? "listening" : "failed to start", cfg.port, cfg.inverterUnitId,
                      g_modbus.unitIdFor(g_modbus.servedDevices() - 1));
        }
        // Which unit is which inverter, once, at boot. Without it the only way to find out is
        // to read modbus.unit_id, fetch /api/v1/devices and count positions -- and on a bus of
        // identical inverters the per-device lines above are indistinguishable (review).
        for (size_t i = 0; i < g_configSlotIds.size() && i < g_modbus.servedDevices(); ++i) {
            log::info("modbus: unit %u -> %s", static_cast<unsigned>(g_modbus.unitIdFor(i)),
                      g_configSlotIds[i].empty() ? "(configured device did not start)"
                                                 : g_configSlotIds[i].c_str());
        }
        if (g_modbus.servedDevices() < g_devicesConfigured) {
            // Which of the two limits was hit, because the fix differs: lowering the base is no
            // help at all when the diagnostics unit is sitting inside the run (validate() only
            // stops it equalling unit_id, not landing a few above it).
            const int firstUnserved = cfg.inverterUnitId + g_modbus.servedDevices();
            log::warn("modbus: only %u of %u configured devices are reachable over Modbus TCP -- "
                      "unit %d is %s. %s",
                      static_cast<unsigned>(g_modbus.servedDevices()),
                      static_cast<unsigned>(g_devicesConfigured), firstUnserved,
                      firstUnserved == cfg.diagnosticsUnitId ? "the diagnostics unit"
                                                             : "past 247, the last valid address",
                      firstUnserved == cfg.diagnosticsUnitId
                          ? "Move modbus.diagnostics_unit_id out of the range."
                          : "Lower modbus.unit_id.");
        }
    }

    if (configSnapshot.mqtt.enabled && !configSnapshot.mqtt.host.empty()) {
        mqtt::MqttConfig cfg;
        cfg.enabled          = true;
        cfg.host             = configSnapshot.mqtt.host;
        cfg.port             = configSnapshot.mqtt.port;
        cfg.username         = configSnapshot.mqtt.username;
        cfg.password         = configSnapshot.mqtt.password;
        cfg.baseTopic        = configSnapshot.mqtt.baseTopic;
        cfg.discoveryPrefix  = configSnapshot.mqtt.discoveryPrefix;
        cfg.discoveryEnabled = configSnapshot.mqtt.discoveryEnabled;
        cfg.qos              = configSnapshot.mqtt.qos;
        g_mqtt               = std::make_unique<mqtt::MqttOutput>(cfg);
        g_mqtt->setDiagnostics(&g_diagnostics);
        // Same function REST submits through -- one counter, one queue, regardless of which
        // transport a command arrived on.
        g_mqtt->setCommandHandler(submitCommand);
        g_mqtt->setCommandOutcomeProvider(
            [](const std::string& requestId) { return g_commandQueue.outcomeFor(requestId); });
        if (g_relays.count() > 0) {
            g_mqtt->setRelayCommandHandler([](uint8_t index, bool on) {
                std::lock_guard<std::mutex> lock(g_relayMutex);
                return g_relays.set(index, on);
            });
            g_mqtt->setDrmCommandHandler(
                [](const std::string& mode) { return applyDrmMode(mode) == CommandResult::Ok; });
        }
        g_mqtt->begin(bridgeInfo());
        // The host is not a secret; the password must never reach a log.
        log::info("mqtt: broker %s:%u", cfg.host.c_str(), cfg.port);
    }

    log::info("web: http://%s/", g_wifi.ipAddress().c_str());
}

void startRestApi() {
    rest::RestContext ctx;
    ctx.devices     = &g_devices;
    ctx.diagnostics = &g_diagnostics;
    ctx.registry    = &g_registry;
    ctx.config      = &g_config;
    // The one sanctioned write path to g_config after boot. The AsyncTCP task publishes a
    // whole new Configuration here while loop()/rs485Task read string members via
    // bridgeInfo(); without the lock that is a use-after-free waiting on a settings save
    // landing mid-poll (review, 2026-07-21).
    ctx.applyConfig = [](const Configuration& c) {
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config = c;
        }
        // Log level is live, not boot-only: the logger is a global whose level is a plain
        // setter. Without this a level change reported "Saved and applied" in the UI but did
        // nothing until a reboot -- and configChangeRequiresReboot() correctly omits it only
        // because this line makes the claim true.
        log::setLevel(c.logLevel);
        // The relay gates follow the config immediately -- no restart. Closing EITHER
        // gate also releases every relay: with the gate closed, no command -- not even
        // OFF -- would get through, so an energised contact would otherwise stay frozen
        // with DRM asserted and no way to release it. The failsafe direction (contacts
        // open, inverter runs) is the only state a closed gate may leave behind.
        {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            g_relays.setReadOnlyMode(c.security.readOnlyMode);
            g_relays.setEnabled(c.relays.enabled);
            if (!c.relays.enabled || c.security.readOnlyMode) {
                g_relays.allOff();
            }
        }
        g_commandDispatcher.setReadOnlyMode(c.security.readOnlyMode);
    };
    ctx.bridgeInfo  = bridgeInfo;
    ctx.clock       = nowMs;
    // Configuration slot -> unit id, which is the mapping only this file knows.
    ctx.modbusUnitIdFor = [](const DeviceId& id) -> uint8_t {
        for (size_t i = 0; i < g_configSlotIds.size(); ++i) {
            if (g_configSlotIds[i] == id) {
                return g_modbus.running() ? g_modbus.unitIdFor(i) : 0;
            }
        }
        return 0;
    };
    // The same table, answering the other question the outside world cannot work out for
    // itself: which configured row a running device came from.
    ctx.configSlotFor = [](const DeviceId& id) -> int {
        for (size_t i = 0; i < g_configSlotIds.size(); ++i) {
            if (g_configSlotIds[i] == id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    ctx.saveConfig  = [](const Configuration& c) { return g_store.save(c); };
    // Request only -- the poll itself runs on rs485Task, exactly like discovery. Running
    // pollOnce() here executed a seconds-long bus transaction inside an AsyncTCP callback and
    // raced the periodic poll for the bus lock: with the mock (no bus) it always won, against
    // the real inverter the wizard's test poll lost and got 409 "bus_busy". Found live during
    // Phase 3 (2026-07-19), same seam class as the deferred reboot above.
    ctx.requestPoll = [] {
        if (!g_context) {
            return false;
        }
        g_manualPollRequested = true;
        return true;
    };
    ctx.requestReboot = [] {
        // Do NOT restart here. This runs inside an AsyncTCP callback and the response has only
        // been *queued*, not sent -- restarting drops the connection before it goes out, and
        // the browser reports a network error for a request that actually succeeded. Worse, a
        // blocking delay() in this callback stalls the whole web server.
        //
        // Hand it to loop() instead, with enough time for the socket to flush.
        g_rebootAtMs = nowMs() + 1500;
    };
    // Symmetric with requestCapture below, and it has to be: rs485Task checks discovery FIRST,
    // so a discovery accepted while a capture was merely pending would jump the queue and the
    // capture would then record the tail of the probe run -- exactly what the guard on the
    // other side exists to prevent. Guarding one direction only left that hole open.
    ctx.requestDiscovery = [](bool extended) {
        // A driver capture is refused here too, and for a reason the other two do not have. It
        // does not want the bus -- it would happily keep recording through a probe sweep. That
        // is the problem: a sweep re-registers every inverter and walks eight addresses, so the
        // recording would be of something no driver ever does, filed under a report that claims
        // to show the driver's own conversation.
        if (g_capture.busy() || g_driverCapture.busy()) {
            return false;
        }
        return g_discovery.request(extended);
    };
    ctx.discoveryReport     = [] { return g_discovery.report(); };
    // Refused while discovery is pending or running as well as while another capture is: both
    // want exclusive use of the same bus. A driver capture blocks it too -- this one takes the
    // bus away from the driver, which is precisely the traffic the other is recording.
    ctx.requestCapture = [](const diag::CaptureConfig& config, const SerialProfile& profile) {
        if (g_discovery.busy() || g_driverCapture.busy()) {
            return false;
        }
        return g_capture.request(config, profile);
    };
    ctx.captureReport = [] { return g_capture.report(); };
    // The mirror of the two above. Nothing else may be holding or about to hold the bus: a
    // recording of a probe sweep or of a passive window (in which the bridge says nothing at
    // all) is not what this report claims to contain.
    ctx.requestDriverCapture = [](const diag::TapConfig& config) {
        if (g_discovery.busy() || g_capture.busy()) {
            return false;
        }
        // The line is not the operator's to choose here -- there is a working driver and it is
        // already talking. Read from the bridge rather than taken from the request, so the
        // report cannot end up describing a line the capture did not run at.
        std::lock_guard<std::mutex> lock(g_configMutex);
        return g_driverCapture.request(config, effectiveSerialProfile());
    };
    ctx.driverCaptureReport = [] { return g_driverCapture.report(); };
    ctx.requestFactoryReset = [] { return g_store.factoryReset(); };
    // The undo behind a configuration restore. Straight through to the store, which owns both
    // the stash and the swap; the REST layer only decides when.
    ctx.stashRollback  = [] { return g_store.stashRollback(); };
    ctx.hasRollback    = [] { return g_store.hasRollback(); };
    ctx.rollbackConfig = [](Configuration& out) {
        const auto result = g_store.rollback(out);
        return result == LoadResult::Ok || result == LoadResult::Migrated;
    };
    // Clears a dump the operator has dealt with. Without it every later diagnostics read keeps
    // reporting the same old crash, and "is this new?" becomes unanswerable. The cached copy is
    // updated in the same breath so the next payload agrees with the flash.
    ctx.clearCoredump = [] {
        if (!diag::eraseCoredump()) {
            return false;
        }
        g_coredump = {};
        return true;
    };
    ctx.portalActive        = [] { return g_wifi.portalActive(); };
    ctx.scanNetworks        = scanNetworksJson;
    // Unconditional, unlike the relay handlers below: commands target inverters, not relays,
    // so they exist regardless of board. The same function MQTT's command topic is wired to.
    ctx.submitCommand  = submitCommand;
    ctx.commandOutcome = [](const std::string& requestId) {
        return g_commandQueue.outcomeFor(requestId);
    };
    if (g_relays.count() > 0) {
        // Behind the same mutex as the MQTT path: REST commands arrive on the AsyncTCP
        // task, MQTT commands on the MQTT task.
        ctx.setRelay = [](uint8_t index, bool on) {
            std::lock_guard<std::mutex> lock(g_relayMutex);
            return g_relays.set(index, on);
        };
        ctx.setDrmMode = applyDrmMode;
    }

    g_rest = std::make_unique<rest::RestApi>(ctx);
    g_rest->begin();
}

/// Clears the retained topics of devices that are no longer where we announced them.
///
/// Retained discovery configs outlive the device that published them, and because availability
/// is bridge-scoped an orphaned entity does not go unavailable -- it reports ONLINE forever with
/// its last value, and anything summing the inverters keeps counting it. What we announced last
/// time is the one fact nothing else survives a reboot knowing.
void reconcileAnnouncedDevices(const BridgeInfo& bridge) {
    // Only when every configured device actually started. g_deviceIds holds the devices that
    // STARTED; one skipped for a duplicate id, a driver that is not compiled in or a full slot
    // table is still configured, and tearing its Home Assistant entities down over a typo that
    // gets corrected in a minute is worse than leaving them one boot longer. Nothing here can
    // know the id of a device that never got a driver -- the id comes from the driver -- so the
    // honest move is to defer the whole reconciliation and leave the bookkeeping untouched
    // (review, 2026-07-26). Zero configured devices lands here too, and never reaches this
    // function at all: with nothing started there is no state and MQTT does not run.
    if (g_deviceIds.size() != g_devicesConfigured) {
        log::warn("MQTT: not clearing retained device topics this boot -- %u of %u configured "
                  "devices started; fix them and reboot",
                  static_cast<unsigned>(g_deviceIds.size()),
                  static_cast<unsigned>(g_devicesConfigured));
        g_announcedReconciled = true;
        return;
    }

    const auto previous = g_store.announcement();
    // Say it, once, if the tree moved. Nothing is cleared automatically: the retained payloads
    // under an old base topic are litter, but the discovery configs under an old prefix are the
    // live Home Assistant entities if only the base topic changed -- deviceUniqueBase() does not
    // include it, so those configs are OVERWRITTEN in place rather than orphaned. An automatic
    // sweep that gets that distinction wrong deletes a working device from someone's dashboard
    // while nobody is watching, which is why no comparable project does this in firmware either
    // (ESPHome ships `esphome clean-mqtt` as a first-party subcommand; Tasmota has none of its
    // own and points at Tasmota Device Manager, a community tool -- both host-side, both run
    // deliberately by a human). What the bridge CAN do that an external script cannot is know where
    // its own topics used to be, so it names them. See docs/mqtt.md and issue #41.
    if (previous.prefixesKnown()) {
        if (previous.baseTopic != g_config.mqtt.baseTopic) {
            log::warn("MQTT: base topic changed %s -> %s; retained payloads under "
                      "%s/%s/... are no longer updated (docs/mqtt.md explains how to clear them)",
                      previous.baseTopic.c_str(), g_config.mqtt.baseTopic.c_str(),
                      previous.baseTopic.c_str(), bridge.bridgeId.c_str());
        }
        if (previous.discoveryPrefix != g_config.mqtt.discoveryPrefix) {
            log::warn("MQTT: discovery prefix changed %s -> %s; Home Assistant will keep the old "
                      "entities until the retained configs under %s/ are cleared",
                      previous.discoveryPrefix.c_str(), g_config.mqtt.discoveryPrefix.c_str(),
                      previous.discoveryPrefix.c_str());
        }
    }

    mqtt::AnnouncementRecord record;
    record.baseTopic       = g_config.mqtt.baseTopic;
    record.discoveryPrefix = g_config.mqtt.discoveryPrefix;
    record.devices.reserve(g_deviceIds.size());
    for (const auto& id : g_deviceIds) {
        record.devices.push_back({id, id == g_primaryDeviceId});
    }

    bool allCleared = true;
    for (const auto& id :
         mqtt::devicesToForget(previous.devices, g_deviceIds, g_primaryDeviceId)) {
        if (!g_mqtt->forgetDevice(id, bridge)) {
            allCleared = false;
            // Kept in the record even though it is gone from the configuration: dropping it
            // here would be the last time anything knew it existed, and its entities would then
            // be orphaned for good.
            record.devices.push_back({id, false});
        }
    }
    if (!allCleared && ++g_announcedAttempts < 5) {
        return;  // link hiccup or a full outbox; try again next pass
    }
    if (!allCleared) {
        log::warn("MQTT: could not clear every removed device's topics; will retry next boot");
    }
    if (!g_store.setAnnouncement(record)) {
        log::warn("MQTT: could not record which devices were announced; removing one later will "
                  "leave its Home Assistant entities behind");
    }
    g_announcedReconciled = true;
}

/// Puts every driver back on the bus and the line back where it belongs.
///
/// Needed after anything that took the bus away from the poll loop and left it changed.
/// Discovery re-registers every inverter and resets the line to the driver's own first
/// profile; a capture leaves the line on whatever the operator asked to listen at. Both end
/// with a bus the driver no longer agrees with, so both end here.
void restoreDriverLine() {
    for (auto& d : g_drivers) {
        d->begin(g_transport);
    }
    if (!g_drivers.empty()) {
        // begin() has just put the line back on the driver's own first profile, exactly as it
        // does at boot. Without this, running discovery from the web UI on a healthy bridge
        // silently undid the stored override and the inverter went quiet until the next power
        // cycle -- the same failure that override exists to prevent, on the one path that
        // reconfigures the line at runtime.
        applySerialOverride();
    }
}

void rs485Task(void* /*arg*/) {
    for (;;) {
        // One feed per iteration. The 120 s budget covers a normal iteration; an extended
        // discovery run no longer fits inside one feed and provides its own, per probe -- see
        // the call below and the watchdog setup in setup().
        esp_task_wdt_reset();
        // Own stack headroom into diagnostics: the 8192 sizing rests on one measured crash
        // (2026-07-19); this keeps creeping growth visible in the API instead of leaving
        // the canary as the only witness. ESP-IDF returns BYTES (StackType_t is uint8_t
        // on xtensa); the scan only walks the unused region -- microseconds.
        g_diagnostics.recordRs485StackFree(
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
        // The driver capture, and note that it does NOT `continue`. Every other branch in this
        // loop takes the iteration away from polling because it needs the bus to itself; this
        // one arms a recorder and gets out of the way, because the poll further down is the
        // thing it exists to record. Taking the iteration would reproduce exactly the bug that
        // issue #62 is about -- a capture that records nothing because it paused the driver
        // whose traffic it wanted.
        //
        // It is also first, so that a capture armed this iteration covers the poll in the same
        // iteration rather than starting one cycle late.
        switch (g_driverCapture.service(g_transport)) {
            case DriverCaptureService::Armed:
                log::info("driver capture: recording the bus for %u s",
                          static_cast<unsigned>(g_driverCapture.report().config.durationMs / 1000));
                break;
            case DriverCaptureService::Collected: {
                const auto report = g_driverCapture.report();
                log::info("driver capture: %u frames (%u sent, %u received), %u bytes",
                          static_cast<unsigned>(report.frames.size()),
                          static_cast<unsigned>(report.txFrames),
                          static_cast<unsigned>(report.rxFrames),
                          static_cast<unsigned>(report.totalBytes));
                break;
            }
            case DriverCaptureService::Idle:
            case DriverCaptureService::Recording:
                break;
        }
        // Discovery first, and instead of polling this cycle: probing re-registers every
        // inverter on the bus, so the two must never interleave. This task owns the bus, which
        // is why the web handler only ever *requests* a run.
        // The watchdog is fed per PROBE, not once per iteration: an extended run now sweeps
        // eight addresses per driver per profile, and on a silent bus every one of those is a
        // response timeout. The one feed above covered a run that was a handful of probes long.
        if (g_discovery.runIfRequested(g_transport, [] { esp_task_wdt_reset(); })) {
            Serial.printf("[discovery] %s\n", g_discovery.report().outcome.reason.c_str());
            // The probe left the bus re-registered and the line on the driver's default; make
            // every device pick that up rather than poll a stale address.
            restoreDriverLine();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        // A capture, on exactly the same terms: instead of this cycle's poll, not alongside it.
        // That is the whole concurrency answer -- there is no iteration in which the bus is
        // being listened to AND polled, so the two cannot interleave by construction.
        //
        // The watchdog is fed per read iteration, not once here: a window is tens of seconds
        // inside this single iteration, which the 120 s budget above would not survive on its
        // own at the maximum window length.
        if (g_capture.runIfRequested(g_transport, [] { esp_task_wdt_reset(); })) {
            const auto report = g_capture.report();
            log::info("capture: %u frames, %u bytes, %u with a valid Modbus CRC",
                      static_cast<unsigned>(report.frames.size()),
                      static_cast<unsigned>(report.totalBytes),
                      static_cast<unsigned>(report.modbusFrames));
            // Only when the line was actually changed. The capture listens at settings the
            // operator chose, which for an unidentified device is precisely NOT the driver's,
            // so leaving them in place would silence a working inverter until the next reboot.
            // But a run that could not take the bus never got that far, and re-running begin()
            // on every driver is a registration handshake on the AA55 family -- real traffic,
            // on a bus that just said it was busy.
            if (report.lineReconfigured) {
                restoreDriverLine();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        // A queued write command, on the same terms as discovery and capture above: instead of
        // this cycle's poll, not alongside it. dispatch() may run a real RS485 transaction
        // (execute() on a writable driver), so it gets the same exclusive slot a poll would --
        // there must never be an iteration that both polls a device and dispatches a command,
        // for the same bus-ownership reason discovery and capture cannot overlap a poll either.
        //
        // REST and MQTT both reach g_commandQueue today (see submitCommand() above) -- what
        // still does NOT exist is a driver that accepts anything put here: every shipping
        // driver's execute() returns CommandResult::Unsupported, so this drains a queue that
        // can genuinely fill but can never yet succeed, following the same request/consume
        // shape already established above for discovery and capture.
        if (auto request = g_commandQueue.take()) {
            DeviceContext* target = nullptr;
            for (size_t i = 0; i < g_deviceIds.size(); ++i) {
                if (g_deviceIds[i] == request->deviceId) {
                    target = g_contexts[i].get();
                    break;
                }
            }
            const DispatchOutcome outcome =
                target != nullptr
                    ? g_commandDispatcher.dispatch(request->command, target->driver())
                    : DispatchOutcome{CommandResult::Rejected,
                                      "unknown device id '" + request->deviceId + "'"};
            log::info("command %s on %s: %s", commandTypeName(request->command.type),
                      request->deviceId.c_str(), outcome.reason.c_str());
            g_commandQueue.recordOutcome(request->command.requestId, outcome);
            continue;
        }
        // At most ONE device per iteration, round-robin. Not a loop over all of them: the
        // watchdog is fed once per iteration, and a bus of eight silent devices would spend
        // eight transaction deadlines back to back before the outputs below ever ran. One per
        // pass keeps the loop's timing independent of how many inverters are chained, and the
        // cursor keeps a device whose backoff has expired from always losing to the one before
        // it in the list.
        //
        // The manual-poll request is honoured by whichever device comes up first. exchange()
        // clears the flag, so one request is still one poll.
        if (!g_contexts.empty()) {
            // A manual poll always goes to device 1, never to whichever device the cursor
            // happens to sit on. The wizard's test poll and POST /actions/poll are both read
            // back through /api/v1/status, which serves the FIRST device -- so sending the poll
            // anywhere else made the button report that nothing had happened.
            if (g_manualPollRequested.exchange(false)) {
                const uint64_t   pollStart = nowMs();
                const PollResult r         = g_contexts.front()->pollOnce();
                if (r == PollResult::Ok) {
                    g_diagnostics.recordPollDuration(
                        static_cast<uint32_t>(nowMs() - pollStart));
                }
                if (r != PollResult::Ok) {
                    log::warn("manual poll of %s: %s", g_deviceIds.front().c_str(),
                              pollResultName(r));
                }
            }
            for (size_t i = 0; i < g_contexts.size(); ++i) {
                const size_t index = (g_pollCursor + i) % g_contexts.size();
                DeviceContext& ctx = *g_contexts[index];
                if (!ctx.due(nowMs())) {
                    continue;
                }
                const uint64_t   pollStart = nowMs();
                const PollResult r         = ctx.pollOnce();
                if (r == PollResult::Ok) {
                    // Wall time of the whole transaction as this task experienced it --
                    // preemption included, which is the point: this is the number that moves
                    // when something else contends for the core or the bus stalls short of a
                    // timeout. Failures are excluded; the snapshot field's comment says why.
                    g_diagnostics.recordPollDuration(
                        static_cast<uint32_t>(nowMs() - pollStart));
                }
                if (r != PollResult::Ok) {
                    // Bounded: one line per attempt, no payload, no growth over time. The device
                    // ID rather than a position: a position among the STARTED devices is not the
                    // position in the config, so "device 2" would name the wrong inverter as
                    // soon as one failed to start -- and it carries the address, which is what
                    // someone standing at the bus actually needs.
                    log::warn("poll %s: %s", g_deviceIds[index].c_str(), pollResultName(r));
                }
                g_pollCursor = (index + 1) % g_contexts.size();
                break;
            }
        }
        // Every output takes every device: MQTT/Home Assistant by topic subtree, REST by device
        // id, Modbus TCP by unit id, Prometheus by a `device` label. docs/architecture.md has
        // the table of what each one did about backwards compatibility for device 1.
        if (g_state) {
            const auto bridge = bridgeInfo();
            const auto diag   = g_diagnostics.snapshot();
            // Snapshots held for the whole block: MqttOutput::DeviceView keeps raw pointers
            // into them, so the shared_ptrs have to outlive the loop() call.
            std::vector<StateHandle>               held;
            std::vector<mqtt::MqttOutput::DeviceView> views;
            held.reserve(g_deviceIds.size());
            views.reserve(g_deviceIds.size());
            for (const auto& id : g_deviceIds) {
                if (StateHandle h = g_devices.state(id)) {
                    held.push_back(std::move(h));
                    // Primary is the CONFIGURED first device, not the first that happened to
                    // start. When it fails to start no device is primary and nobody inherits
                    // its topics, its Home Assistant entities or its history.
                    views.push_back({id, held.back().get(), id == g_primaryDeviceId});
                }
            }
            const auto first = g_state->snapshot();
            // Every device, one Modbus unit id each: configuration slot i is served at
            // inverterUnitId + i (#36).
            //
            // Keyed on g_configSlotIds -- the CONFIGURED positions -- not on g_deviceIds, which
            // is compacted. A device that failed to start is absent from the latter, so keying
            // on it moved every later inverter down a unit id: a client reading unit 2 would
            // get inverter 3's watts with no way to notice, and a unit id is a wire contract
            // nobody re-derives after bring-up. A slot with no device is passed as null;
            // refresh() leaves its map at the constructed sentinels, so that unit answers
            // offline with no readings rather than someone else's.
            std::vector<const DeviceState*> modbusDevices;
            modbusDevices.reserve(g_configSlotIds.size());
            for (const auto& slotId : g_configSlotIds) {
                const auto it = std::find_if(views.begin(), views.end(),
                                             [&slotId](const mqtt::MqttOutput::DeviceView& v) {
                                                 return !slotId.empty() && v.id == slotId;
                                             });
                modbusDevices.push_back(it == views.end() ? nullptr : it->state);
            }
            g_modbus.refresh(modbusDevices, bridge, diag, nowMs());
            if (g_mqtt) {
                // Once, on the first connected pass -- before loop() below, so the clears are
                // queued ahead of this boot's own discovery announcements.
                if (!g_announcedReconciled && g_mqtt->connected()) {
                    reconcileAnnouncedDevices(bridge);
                }
                g_mqtt->loop(views, bridge, diag, nowMs());
            }
            if (g_rest) {
                g_rest->notifyState(*first, nowMs());
            }
        }
        // Never a long delay: an unreachable inverter must not stall the task or the watchdog.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

void setup() {
    // No GPIO forcing here. Earlier revisions drove GPIO47 low first thing, believing this
    // board was the Relay-1CH and that pin its relay. The real board (RS485-CAN) has no
    // relay, and the safest state for a pin with no known function is untouched hi-Z.
    // First thing, before anything that could crash: a boot that dies during setup still
    // leaves its predecessor's death recorded, and its own becomes the next entry.
    g_bootRecord = breadcrumbs::begin(
        g_breadcrumbStore, (static_cast<uint32_t>(kFirmwareMajor) << 16) |
                               (static_cast<uint32_t>(kFirmwareMinor) << 8) | kFirmwarePatch);

    Serial.begin(115200);
    const uint32_t serialDeadline = millis() + 2000;
    while (!Serial && millis() < serialDeadline) {
        delay(10);
    }
    // Headless operation is the normal state: no USB host for months. HWCDC's default TX
    // timeout is 100 ms PER WRITE once the 256-byte ring fills with nobody draining it, and
    // the logger runs on rs485Task too -- every log line would then stall polling for up to
    // that timeout. Zero means drop-when-full: the REST log ring keeps every line anyway.
    Serial.setTxTimeoutMs(0);

    Serial.printf("\nHeliograph %s\nboard: %s\nreset reason: %d\n", kFirmwareVersion,
                  board::kName, static_cast<int>(esp_reset_reason()));

    // From the very first log line: uptime stamps until the clock is valid, wall-clock the
    // moment it is -- which on RTC boards is seconds from now, long before any network.
    TimeManager::installLogTimestamps();

    const auto loaded = g_store.load(g_config);
    // Apply the stored level before anything else logs, or the first lines ignore it.
    log::setLevel(g_config.logLevel);

    // TZ before the FIRST log:: call, not merely before the RTC restore. formatLogTimestamp
    // renders through localtime_r, so a stamped line written while TZ is unset comes out in
    // UTC with nothing saying so. That is not hypothetical: a warm reset (OTA reboot, the
    // reboot action) keeps the system clock across the restart, so the clock is already valid
    // here and the config line below shipped an hour or two in the past on every OTA -- the
    // exact boot whose log gets read. Seen on 0.13.0, 2026-07-26. A cold start hid it: the
    // clock is not valid yet at this point, so the line carries an uptime stamp instead.
    //
    // This is as early as it can go. The zone comes out of the stored configuration, so the
    // load above has to have happened -- which is fine, because nothing on that path logs.
    setenv("TZ", g_config.ntp.timezone.c_str(), 1);
    tzset();

    log::info("config: %s (log level %s)", loadResultName(loaded), logLevelName(g_config.logLevel));

    // Once per boot, before anything else can panic on top of it. A dump here means the PREVIOUS
    // run crashed; saying so in the boot log is half the value, because that log is what gets
    // read after an unexplained reboot.
    g_coredump = diag::readCoredumpSummary();
    if (g_coredump.present) {
        log::warn("coredump: previous boot crashed in task '%s' at pc 0x%08lX "
                  "(clear it from Diagnostics once handled)",
                  g_coredump.taskName.empty() ? "?" : g_coredump.taskName.c_str(),
                  static_cast<unsigned long>(g_coredump.programCounter));
    }
    if (loaded == LoadResult::Corrupt || loaded == LoadResult::FutureVersion) {
        // Defaults, so the device lands in the setup portal. Better than running on values we
        // could not parse or do not understand.
        Serial.println("[config] falling back to defaults; the setup portal will start");
    }

    // RTC restore, before anything else that logs at length: with a battery-backed
    // PCF85063 (board::kHasRtc) the system clock is valid from here on, so every log line
    // below carries a wall-clock timestamp even when the network never comes up -- which
    // is exactly the boot you end up debugging.
    if (rtc::begin()) {
        time_t stored = 0;
        if (rtc::readUtc(stored)) {
            const timeval tv{stored, 0};
            settimeofday(&tv, nullptr);
            char         buf[24];
            const size_t n = log::formatIsoLocalTime(buf, sizeof(buf), stored);
            // formatIsoLocalTime leaves buf untouched when localtime_r rejects the epoch or
            // strftime does not fit, so printing it unconditionally would read uninitialised
            // stack. Same guard as the NTP-sync line in time_manager.cpp.
            log::info("rtc: clock restored: %s (awaiting ntp for drift correction)",
                      n > 0 ? buf : "?");
        } else {
            // Deliberately does not name a cause. readUtc() refuses for five distinct reasons
            // -- oscillator stopped, a byte that is not valid BCD, a field out of range, a
            // year outside 2024-2099, or a date that does not exist -- and only the first is
            // "flat backup cell". Naming that one sent someone to replace a battery over what
            // was actually a wiring or I2C fault (review, 2026-07-25).
            log::warn("rtc: present but gave no usable time; running without it until ntp "
                      "(flat backup cell, or a bad read -- check the I2C wiring if it persists)");
        }
    }

    // Relays, on boards that have them: pins to OUTPUT and everything de-energised before
    // anything else can fail. The gates start closed (read-only on, enabled off) and only
    // the config below opens them. Under HELIOGRAPH_MOCK_RELAYS the mock build exposes
    // virtual relays through the full MQTT/REST/HA stack without touching a single pin.
#if defined(HELIOGRAPH_MOCK_RELAYS)
    g_relays.begin(HELIOGRAPH_MOCK_RELAYS, [](uint8_t i, bool on) {
        log::info("relay[mock] %u -> %s", i + 1, on ? "ON" : "OFF");
    });
#else
    if (board::kRelayCount > 0) {
        for (int i = 0; i < board::kRelayCount; ++i) {
            pinMode(board::kRelayPins[i], OUTPUT);
        }
        g_relays.begin(static_cast<uint8_t>(board::kRelayCount), [](uint8_t i, bool on) {
            digitalWrite(board::kRelayPins[i],
                         (on == board::kRelayActiveHigh) ? HIGH : LOW);
        });
    }
#endif
    g_relays.setReadOnlyMode(g_config.security.readOnlyMode);
    g_relays.setEnabled(g_config.relays.enabled);
    g_commandDispatcher.setReadOnlyMode(g_config.security.readOnlyMode);

    // BOOT button, status LED and buzzer on boards that have them (6CH). No-op elsewhere.
    initOnboardIndicators();

    // A factory-fresh device gets a UNIQUE default hostname (heliograph-a1b2c3, from the
    // MAC) instead of the shared "heliograph": two bridges on one LAN -- configuring a
    // second unit at home before installing it elsewhere is the normal way to deploy one --
    // otherwise fight over the same mDNS name. Provisioned devices keep whatever hostname
    // they were given; this only upgrades the never-configured default.
    if (!g_config.provisioned() && g_config.wifi.hostname == "heliograph") {
        g_config.wifi.hostname = g_wifi.bridgeId();
    }

    registerBuiltinDrivers(g_registry);
    g_wifi.setDiagnostics(&g_diagnostics);
    // lwip only harvests the DHCP-offered NTP server (option 42) if this is armed when the
    // lease arrives -- but the arming call is dispatched via the tcpip thread, so it must
    // run AFTER the network stack exists and BEFORE the lease: exactly the hook's moment.
    // Calling it here directly, before WiFi, aborted the boot (0.4.4, 2026-07-21).
    g_wifi.setNetworkStackReadyHook([] { g_time.prepareDhcp(g_config); });
    g_wifi.begin(g_config);
    if (!g_config.provisioned()) {
        Serial.printf("[wifi] not provisioned; setup AP '%s' is up\n", g_wifi.apSsid().c_str());
    }

    // The driver starts regardless of the network: RS485 does not need WiFi, and a bridge
    // sitting in the setup portal should already be polling so the first page load has real
    // data on it.
    const std::string driverId = selectedDriverId();
    // Pass the configured driver options through: a unit_id or profile set in the web UI
    // must reach the driver, not silently fall back to factory defaults (2026-07-21 review).
    // Device 1 comes from `driver`, the rest from `additional_devices`, in that order. One list
    // so the poll loop has one thing to walk, and so a bring-up log reads in the same order the
    // settings page shows.
    // The plan -- which devices to start and which to refuse -- is decided in app/device_plan,
    // where the host build can reach it. It used to be decided here, in the one file no test
    // compiles, and the rule it enforces is the one that keeps a second instance of a
    // single-device driver from wiping the first one's address off the bus.
    const std::vector<app::PlannedDevice> planned =
        app::planDevices(g_config, driverId, g_registry);
    g_devicesConfigured = planned.size();
    // One slot per CONFIGURED device, filled in as each one starts. The Modbus unit ids are
    // keyed on this, not on the compacted g_deviceIds -- see the declaration.
    g_configSlotIds.assign(planned.size(), DeviceId{});
    if (planned.empty()) {
        log::warn("no driver configured and none compiled in; nothing will be polled");
    }

    for (size_t plannedIndex = 0; plannedIndex < planned.size(); ++plannedIndex) {
        const auto& p = planned[plannedIndex];
        // "Device 1" is the `driver` section; 2..N are additional_devices, which is how the
        // settings page numbers them. Naming only the driver id was useless on the very bus
        // this exists for: three inverters share one driver id, so all three failures read
        // identically (review, 2026-07-25).
        // The label goes in the PROBLEM text too. "device 2 could not be started" sends someone
        // to count rows on the settings page; "device 2 (Schuur) could not be started" sends
        // them to the shed. The row number stays, because that is what the settings page shows
        // and an unlabelled bridge still needs to be told which row.
        const std::string row = app::describeRow(p.row, p.label);

        // The refusal was decided in planDevices(), before anything was created, because for a
        // driver that cannot share a bus begin() IS the damage: the AA55 handshake opens with a
        // bus-wide RE_REGISTER, so a second instance tells the first, already-polling inverter
        // to forget its address (#63). What is left here is REPORTING it -- the half that needs
        // a serial port and a problems list, and the half that was never the interesting one.
        if (!p.shouldStart()) {
            log::warn("device '%s' skipped: this driver supports only one device per bridge",
                      p.id.c_str());
            g_deviceProblems.push_back(p.problem);
            continue;
        }

        auto driver = g_registry.create(p.id, g_transport, *p.options);
        if (!driver || !driver->begin(g_transport)) {
            // Named, and the loop continues: one unconfigurable device must not cost the others
            // their poll. A bus with three inverters where the second has a typo'd driver id
            // should still report the first and third.
            log::warn("device '%s' could not be started; the others still poll", p.id.c_str());
            g_deviceProblems.push_back(row + " ('" + p.id + "') could not be started");
            continue;
        }
        Serial.printf("[driver] %s (%s)\n", driver->descriptor().id.c_str(),
                      supportLevelName(driver->descriptor().supportLevel));
        const DeviceId id = driver->identity().deviceId();
        // Checked BEFORE add(), because add() is idempotent: re-adding a known id hands back
        // the existing store rather than refusing. That is right for a caller re-registering
        // the same device, and exactly wrong here -- two configured devices sharing an id would
        // have silently shared one store and overwritten each other's readings into one set of
        // Home Assistant entities. An earlier version of this loop treated a null return as the
        // collision signal; it never came (review, 2026-07-25).
        if (g_devices.contains(id)) {
            log::warn("device '%s' skipped: another configured device already resolves to id "
                      "'%s' -- give them different addresses", p.id.c_str(), id.c_str());
            g_deviceProblems.push_back(row + " ('" + p.id + "') resolves to " + id +
                                       ", which another configured device already uses; give "
                                       "them different addresses");
            continue;
        }
        StateStore* store = g_devices.add(id);
        if (store == nullptr) {
            log::warn("device '%s' skipped: no free device slot (max %u)", p.id.c_str(),
                      static_cast<unsigned>(kMaxDevices));
            g_deviceProblems.push_back(row + " ('" + p.id + "') has no free device slot");
            continue;
        }
        PollPolicy policy;
        policy.intervalMs = g_config.polling.intervalSeconds * 1000;
        g_contexts.push_back(std::make_unique<DeviceContext>(*driver, *store, g_diagnostics,
                                                             nowMs, policy, p.label));
        g_deviceIds.push_back(id);
        g_configSlotIds[plannedIndex] = id;
        // `planned` is in configuration order and `p` is the entry being started, so this is
        // true only for the first configured device -- never for a later one that starts first.
        if (&p == &planned.front()) {
            g_primaryDeviceId = id;
        }
        g_drivers.push_back(std::move(driver));
        if (g_driver == nullptr) {
            g_driver  = g_drivers.front().get();
            g_context = g_contexts.front().get();
            g_state   = store;
        }
    }
    if (!g_drivers.empty()) {
        // Once, after the LAST begin(): every begin() reconfigures the line to its own driver's
        // first profile, so applying the override per device would only be undone by the next
        // one. All devices share the bus, so there is one line to set, not one per device --
        // which also means that on a MIXED bus, with the override off, the line is left on the
        // last driver's profile. Drivers whose profiles differ need the override set explicitly.
        applySerialOverride();
        if (g_drivers.size() > 1) {
            Serial.printf("[driver] polling %u devices in turn on one bus\n",
                          static_cast<unsigned>(g_drivers.size()));
        }
    }

    // The web server runs on the portal AP too -- that is how setup happens at all.
    startRestApi();

    // Pinned to core 1: WiFi and lwIP live on core 0, so network load cannot disturb RS485
    // timing. See docs/architecture.md.
    //
    // 8192, not 4096: this task runs the whole driver chain, and the deepest real path --
    // poll -> registerDevice -> transact (two ~300 B frame buffers) -> traceHex -> emit ->
    // newlib vsnprintf (~1.3 KB by itself) -- blew the 4 KB canary within seconds of the
    // first contact with a real inverter (boot loop, 2026-07-19). The mock never came close:
    // its poll is pure arithmetic and never touches the transport or the hexdump path.
    TaskHandle_t rs485Handle = nullptr;
    xTaskCreatePinnedToCore(rs485Task, "rs485", 8192, nullptr, 5, &rs485Handle, 1);

    // Watchdog coverage for both application tasks. Without this, only the idle tasks were
    // watched: a HANG (as opposed to a crash) in loop() or rs485Task ran forever with no
    // recovery -- and a hang after the OTA image was confirmed healthy is permanent, because
    // a never-resetting device also never rolls back (review, 2026-07-21). 120 s, not the
    // 5 s default: an extended discovery scan legitimately runs many back-to-back 3 s
    // transactions between feeds, and the purpose here is catching forever-hangs, not
    // latency policing.
    //
    // Side effect worth naming: esp_task_wdt_reconfigure applies to EVERY watched task,
    // including the core-0 idle task kept by idle_core_mask. So idle starvation on core 0,
    // which the IDF default would have caught in 5 s, now takes two minutes to trip. The API
    // has no per-task timeout, so the choice is this or no idle coverage at all; discovery
    // needs the headroom more than idle starvation needs the faster trip.
    //
    // Not watched at all: the AsyncTCP task behind the web server and the task espMqttClient
    // creates for itself. Registering a task whose blocking behaviour we do not control risks
    // panicking a healthy device, so their liveness is reported through diagnostics instead
    // (webLastServiceMs / mqttLastServiceMs) and left for a human or an alert to act on.
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms    = 120000,
        .idle_core_mask = 1 << 0,  // keep the idle-task coverage the sdkconfig had
        .trigger_panic = true,     // panic -> reset -> (unconfirmed image) -> rollback
    };
    esp_task_wdt_reconfigure(&wdtConfig);
    enableLoopWDT();
    if (rs485Handle != nullptr) {
        esp_task_wdt_add(rs485Handle);
    }
}

void loop() {
    if (g_rebootAtMs != 0 && nowMs() >= g_rebootAtMs) {
        Serial.println("[sys] rebooting");
        Serial.flush();
        ESP.restart();
    }

    // Confirm a freshly-flashed image to the bootloader once it has run healthily, so it is not
    // rolled back on the next reboot. Latched: the check runs until it fires once. Gated on
    // network health, not on an inverter poll -- the inverter is gone every night.
    static bool bootConfirmed = false;
    if (ota::shouldConfirmHealthyBoot(g_wifi.connected(), nowMs(), bootConfirmed,
                                      g_diagnostics.pollSuccessTotal() > 0)) {
        ota::confirmHealthyBoot();
        bootConfirmed = true;
        log::info("ota: image confirmed healthy; rollback cancelled");
    }

    // Breadcrumb heartbeat: "this life reached this uptime". Throttled inside to one RTC-RAM
    // write per second.
    breadcrumbs::tick(g_breadcrumbStore, static_cast<uint32_t>(nowMs()));

    g_wifi.loop(nowMs());
    startOutputs();  // no-op until there is a network, and only ever runs once
    serviceOnboard();  // BOOT-hold factory reset + status LED (no-op on boards without them)

    // After every NTP sync, put the corrected time into the battery-backed RTC (when the
    // board has one), so the next boot restores an accurate clock. Runs on the loop task,
    // not in the SNTP callback: I2C from lwip's thread is asking for trouble.
    static time_t lastRtcSync = 0;
    const time_t  ntpSync     = g_time.lastSyncEpoch();
    if (ntpSync != 0 && ntpSync != lastRtcSync) {
        lastRtcSync = ntpSync;
        if (rtc::writeUtc(time(nullptr))) {
            log::debug("rtc: updated from ntp");
        }
    }

    // modbus_client_connections_total was a dead counter: recordModbusClient() existed but
    // nothing ever called it, so the API reported 0 forever (found by the Fase 9 multi-client
    // test, 2026-07-22: three live clients, counter stayed 0). eModbus exposes no connect
    // hook, so count rising edges of the active-client count instead. Sampled per loop pass;
    // a connection shorter than one pass can be missed, which is fine for a trend counter.
    {
        static uint16_t prevModbusClients = 0;
        const uint16_t  nowClients        = g_modbus.activeClients();
        for (uint16_t i = prevModbusClients; i < nowClients; ++i) {
            g_diagnostics.recordModbusClient();
        }
        // Say when the ceiling is reached, because eModbus will not: past maxNoClients it
        // accepts the socket, closes it and logs nothing (ModbusServerTCPasync::onClientConnect).
        // From outside that is a client which connects and is never answered, and the only way
        // to find out why is to go and read eModbus's source -- which is exactly what the
        // 4-of-6 hardware result on 2026-07-27 cost (#71).
        //
        // This reports being AT the limit, not a refusal. Counting refusals would need a hook
        // eModbus does not offer. Being at the limit is the condition under which the next
        // connection is refused, which is the part worth knowing in advance.
        //
        // Edge-triggered: the limit stays reached for as long as the clients stay connected,
        // and a warning every loop pass would bury the log it belongs in.
        const uint16_t limit = g_modbus.config().maxClients;
        if (nowClients >= limit && prevModbusClients < limit) {
            log::warn("modbus: %u of %u client slots in use -- further connections are refused "
                      "until one frees. Raise modbus.max_clients if this is normal here.",
                      static_cast<unsigned>(nowClients), static_cast<unsigned>(limit));
        }
        prevModbusClients = nowClients;
    }

    static uint32_t lastReport = 0;
    if (millis() - lastReport > 10000) {
        lastReport = millis();
        // Same per-task self-report as rs485Task; see the note there.
        g_diagnostics.recordLoopStackFree(
            static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
        // THE WHOLE FLEET, not the first device (#74). This used to read g_state, which is
        // assigned once while starting devices and then holds device 1 forever: on a bus of
        // four, three could be dead and this line would report the healthy one every ten
        // seconds. It also printed `inverter=%d` from a bool, so "1" meant online and read as
        // a count -- identical output for one inverter and for eight.
        //
        // Summed by rest::totalsFor so the heartbeat and /api/v1/status cannot disagree about
        // what "answering" means.
        //
        // Through the logger, not a raw print: the heartbeat then carries the same wall-clock
        // stamp as everything else, which is what makes an unattended capture legible.
        {
            const uint64_t                   now = nowMs();
            std::vector<rest::DeviceSummary> fleet;
            fleet.reserve(g_deviceIds.size());
            for (const auto& id : g_deviceIds) {
                if (StateHandle h = g_devices.state(id)) {
                    fleet.push_back(rest::summariseDevice(*h, id, now));
                }
            }
            const auto t = rest::totalsFor(fleet);
            log::info("state: wifi=%s devices=%u/%u answering power=%s heap=%lu",
                      provisioningStateName(g_wifi.state()), t.answering,
                      static_cast<unsigned>(g_devicesConfigured),
                      t.acCount != 0 ? String(t.acPowerW, 1).c_str() : "unknown",
                      static_cast<unsigned long>(ESP.getFreeHeap()));
            // One line each for the devices that are NOT answering, and none at all when the
            // fleet is healthy. A capture from a working bridge stays one line per heartbeat;
            // a sick one names the inverter instead of leaving it to be deduced from a count.
            // Bounded by kMaxDevices, so this can never be more than eight lines.
            for (const auto& f : fleet) {
                if (f.online && f.dataValid && !f.dataStale) {
                    continue;
                }
                if (f.everPolled) {
                    // WHY, not just that. The old line carried valid= and stale= for the first
                    // device, and folding three causes into one phrase would have dropped what
                    // those flags were for. Offline before stale: a device that drops is marked
                    // offline AND stale by markAllStale(), and the offline is the cause.
                    const char* why = !f.online          ? "offline"
                                      : f.dataStale      ? "stale"
                                                         : "no valid reading";
                    log::info("state:   %s not answering (%s, last reply %us ago)",
                              rest::displayName(f).c_str(), why,
                              static_cast<unsigned>(f.lastPollSecondsAgo));
                } else {
                    // Never a byte since boot: a bus or addressing fault, not an inverter that
                    // went quiet, and the two need different things done about them.
                    log::info("state:   %s has never answered", rest::displayName(f).c_str());
                }
            }
            // A device that never STARTED is not in the fleet at all -- it has no driver, so it
            // has no id to print (see the reconciliation note above). Without this the count
            // would read "3/4" with nothing named, which is the one case a reader cannot tell
            // apart from a bug in the count. Said at boot too, but a capture taken hours later
            // has long since lost that line out of the ring.
            for (const auto& p : g_deviceProblems) {
                log::info("state:   %s", p.c_str());
            }
        }
    }
    delay(100);
}
