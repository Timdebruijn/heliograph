// SPDX-License-Identifier: MIT
// Discovery scoring, and above all: when NOT to decide.

#include <unity.h>

#include <string>

#include "app/discovery_runner.h"
#include "drivers/discovery_engine.h"
#include "drivers/driver_registry.h"
#include "support/mock_transport.h"

using namespace heliograph;
using test::MockTransport;

static uint64_t g_now = 0;
static uint64_t clockFn() { return g_now; }

void setUp() { g_now = 1000; }
void tearDown() {}

/// A driver that reports whatever the test tells it to. Lets the scoring rules be tested
/// without any protocol in the way.
class FakeDriver : public InverterDriver {
public:
    struct Script {
        bool        responded  = true;
        bool        checksumValid = true;
        int         score      = 0;
        std::string serial     = "SER-1";
        std::string model      = "Model-1";
        /// Second and later probes report this serial instead, simulating an unstable match.
        std::string serialOnRepeat;
        bool        writeAttempted = false;
        /// Non-zero: begin() reconfigures the line to this, as every real driver does.
        uint32_t    begunAtBaud = 0;
        /// Non-zero: probe() only answers when `bus` is actually at this rate. `bus` is the
        /// test's own MockTransport -- the base Transport exposes no way to read back the
        /// configured line settings, and adding one just for a test would be the tail wagging
        /// the dog.
        uint32_t             respondsOnlyAtBaud = 0;
        const MockTransport* bus                = nullptr;
    };

    FakeDriver(DriverDescriptor d, Script* script) : descriptor_(std::move(d)), script_(script) {}

    const DriverDescriptor& descriptor() const override { return descriptor_; }

    // Real drivers configure the UART here, from their own first recommended profile, because a
    // driver started straight from config must not depend on a discovery run having done it.
    // Modelled so the profile sweep is tested against the behaviour it actually has to survive.
    bool begin(Transport& t) override {
        if (script_->begunAtBaud != 0) {
            SerialProfile own;
            own.baudRate = script_->begunAtBaud;
            t.configure(own);
        }
        return true;
    }

    ProbeResult probe() override {
        ++probeCount_;
        ProbeResult r;
        // A device only answers at the line rate it actually runs at.
        if (script_->respondsOnlyAtBaud != 0 && script_->bus != nullptr &&
            script_->bus->profile().baudRate != script_->respondsOnlyAtBaud) {
            r.responded = false;
            r.evidence.push_back("wrong baud");
            return r;
        }
        r.responded      = script_->responded;
        r.checksumValid  = script_->checksumValid;
        r.confidenceScore = script_->score;
        r.detectedModel  = script_->model;
        r.serialNumber = (probeCount_ > 1 && !script_->serialOnRepeat.empty())
                             ? script_->serialOnRepeat
                             : script_->serial;
        r.evidence.push_back("fake probe");
        return r;
    }

    PollResult           poll(DeviceState&) override { return PollResult::Ok; }
    DeviceIdentity       identity() const override { return {}; }
    InverterCapabilities capabilities() const override { return {}; }
    BusErrorCounts       busErrors() const override { return {}; }
    CommandResult        execute(const InverterCommand&) override {
        script_->writeAttempted = true;  // discovery must never reach this
        return CommandResult::Ok;
    }

private:
    DriverDescriptor descriptor_;
    Script*          script_;
    int              probeCount_ = 0;
};

static DriverDescriptor desc(const std::string& id, int priority, bool autoDetect = true) {
    DriverDescriptor d;
    d.id                    = id;
    d.displayName           = id;
    d.supportedTransports   = {TransportType::Mock};
    d.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000}};
    d.probePriority         = priority;
    d.supportsAutoDetection = autoDetect;
    return d;
}

static void addDriver(DriverRegistry& r, const DriverDescriptor& d, FakeDriver::Script* script) {
    r.registerDriver(d,
                     [d, script](Transport&, const DriverOptions&) -> std::unique_ptr<InverterDriver> {
                         return std::make_unique<FakeDriver>(d, script);
                     });
}

// --- a bus with several devices on it --------------------------------------------------------
//
// Discovery created every candidate driver with no options, so every Modbus driver probed at
// its declared default unit id and nothing else. On a chain of identical inverters the wizard's
// answer to "which inverters are on this bus" was "there is one at address 1" (#37).

/// A bus where devices sit at fixed addresses. The driver is built with the options the engine
/// probed with, so a probe only answers when something is actually at that address.
struct AddressBus {
    std::vector<std::string> occupied;
    /// Every address probed, in order. The cost of a sweep is a property worth pinning: this
    /// runs on a live RS485 bus with the poll loop stopped.
    std::vector<std::string> probed;
    /// Same serial at every address, for the one-device-two-addresses case.
    bool sharedSerial = false;
};

class AddressedDriver : public InverterDriver {
public:
    AddressedDriver(DriverDescriptor d, AddressBus* bus, std::string address)
        : descriptor_(std::move(d)), bus_(bus), address_(std::move(address)) {}

    const DriverDescriptor& descriptor() const override { return descriptor_; }
    bool                    begin(Transport&) override { return true; }

    ProbeResult probe() override {
        if (!counted_) {
            bus_->probed.push_back(address_);
            counted_ = true;  // the consistency re-probe is the same address, not a new one
        }
        ProbeResult r;
        r.responded = std::find(bus_->occupied.begin(), bus_->occupied.end(), address_) !=
                      bus_->occupied.end();
        if (!r.responded) {
            return r;
        }
        r.checksumValid   = true;
        r.confidenceScore = 95;
        r.detectedModel   = "MIC-TL-X";
        r.serialNumber    = bus_->sharedSerial ? "SHARED" : "SER-" + address_;
        return r;
    }

    PollResult           poll(DeviceState&) override { return PollResult::Ok; }
    DeviceIdentity       identity() const override { return {}; }
    InverterCapabilities capabilities() const override { return {}; }
    BusErrorCounts       busErrors() const override { return {}; }
    CommandResult        execute(const InverterCommand&) override { return CommandResult::Ok; }

private:
    DriverDescriptor descriptor_;
    AddressBus*      bus_;
    std::string      address_;
    bool             counted_ = false;
};

static DriverDescriptor addressedDesc(const std::string& id, const std::string& defaultAddress,
                                      long minValue = 1, long maxValue = 247) {
    DriverDescriptor d      = desc(id, 10);
    d.addressOptionKey      = "unit_id";
    d.options               = {DriverOption{"unit_id", "Unit id", "", defaultAddress, {},
                                            minValue, maxValue}};
    return d;
}

static void addAddressedDriver(DriverRegistry& r, const DriverDescriptor& d, AddressBus* bus) {
    r.registerDriver(d, [d, bus](Transport&,
                                 const DriverOptions& options) -> std::unique_ptr<InverterDriver> {
        // Falls back to the declared default exactly as every real driver's optionsFrom() does.
        // Without that, "probed with no options" would look like "probed at no address", and
        // the quick-mode test below would be asserting the fake rather than the engine.
        return std::make_unique<AddressedDriver>(d, bus, d.optionOr(options, "unit_id"));
    });
}

static void test_extended_discovery_finds_devices_at_other_addresses() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"1", "2", "3"};  // three identical inverters, the bus this exists for
    addAddressedDriver(reg, addressedDesc("growatt_like", "1"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(3, out.candidates.size());
    std::vector<std::string> found;
    for (const auto& c : out.candidates) {
        found.push_back(c.address());
    }
    std::sort(found.begin(), found.end());
    TEST_ASSERT_EQUAL_STRING("1", found[0].c_str());
    TEST_ASSERT_EQUAL_STRING("2", found[1].c_str());
    TEST_ASSERT_EQUAL_STRING("3", found[2].c_str());
    // And the option map is what the wizard configures, so it has to carry the address.
    TEST_ASSERT_EQUAL_STRING("1", out.selectedOptions.at("unit_id").c_str());
}

// Quick mode is the one that runs by default and on a bus that may already be in service.
// Its cost must not change: one probe per driver, at the driver's own default.
//
// And it must carry NO options. Passing the driver's default looks equivalent -- same value,
// same probe -- but the wizard prefers a discovered address over the stored configuration, so a
// quick candidate claiming an address it never discovered would propose the default to a bridge
// configured at unit id 7 (review, 2026-07-26).
static void test_quick_discovery_probes_the_default_address_and_claims_nothing() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"1", "2", "3"};
    addAddressedDriver(reg, addressedDesc("growatt_like", "1"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_EQUAL_size_t(1, bus.probed.size());
    TEST_ASSERT_EQUAL_STRING("1", bus.probed[0].c_str());  // its own default, internally
    TEST_ASSERT_TRUE_MESSAGE(out.candidates[0].matchedOptions.empty(),
                             "a quick candidate must not claim an address it did not discover");
    TEST_ASSERT_TRUE(out.candidates[0].address().empty());
    TEST_ASSERT_TRUE(out.selectedOptions.empty());
    TEST_ASSERT_TRUE(out.sweptAddresses.empty());
}

// The driver's own default is always tried, whatever the sweep set says. SolaX registers at 10,
// which is outside 1..8 -- a wizard that stopped finding the device it used to find would be a
// regression dressed as a feature.
static void test_the_drivers_own_default_is_probed_even_outside_the_sweep() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"10"};
    addAddressedDriver(reg, addressedDesc("solax_like", "10"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_EQUAL_STRING("10", out.candidates[0].address().c_str());
    TEST_ASSERT_EQUAL_STRING("10", bus.probed[0].c_str());  // its own default first
}

// The option bounds are the protocol's. Probing outside them is transactions the driver would
// refuse anyway, on someone's live bus.
static void test_the_sweep_stays_inside_the_declared_bounds() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"2"};
    addAddressedDriver(reg, addressedDesc("small_range", "1", 1, 3), &bus);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(3, bus.probed.size());  // 1, 2, 3 -- not 1..8
    TEST_ASSERT_EQUAL_STRING("3", bus.probed.back().c_str());
}

// A protocol that addresses devices itself has nothing to sweep, and must not be probed eight
// times to establish that.
static void test_a_driver_without_an_address_option_is_probed_once() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("eversolar_like", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_TRUE(out.candidates[0].address().empty());
    TEST_ASSERT_TRUE(out.candidates[0].matchedOptions.empty());
}

// The margin rule guards against ambiguity about which PROTOCOL a device speaks. Three
// inverters of the same make are not ambiguous -- and reading them as "too close to call" would
// have made the sweep defeat auto-selection on exactly the bus it was built for.
static void test_identical_inverters_do_not_block_auto_selection() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"1", "2", "3"};
    addAddressedDriver(reg, addressedDesc("growatt_like", "1"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("growatt_like", out.selectedDriverId.c_str());
    // ...and the owner is told the other two exist, because the wizard configures one device.
    TEST_ASSERT_TRUE(out.reason.find("3 devices answered") != std::string::npos);
}

// Two addresses, one serial number: an inverter mid-reconfiguration, or one still answering on
// an address it was moved off. Two entries would have the owner configure it twice, and two
// configured devices resolving to one physical inverter is the collision the boot loop refuses.
static void test_one_device_answering_at_two_addresses_is_reported_once() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied     = {"1", "5"};
    bus.sharedSerial = true;
    addAddressedDriver(reg, addressedDesc("growatt_like", "1"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_EQUAL_STRING("1", out.candidates[0].address().c_str());
    bool mentioned = false;
    for (const auto& line : out.candidates[0].probe.evidence) {
        mentioned = mentioned || line.find("also answered at address 5") != std::string::npos;
    }
    TEST_ASSERT_TRUE_MESSAGE(mentioned, "the second address has to be reported, not dropped");
}

// Two units left on one address answer together and their replies collide into a bad checksum.
// The probe reports that as "did not respond" -- deliberately, so garbage at the wrong baud rate
// cannot abort the profile sweep -- and it used to be discarded with the failed probe. It is the
// one fault a sweep can name that nothing else can, and it is the likeliest mistake on a chain
// of identical inverters.
class NoisyDriver : public InverterDriver {
public:
    NoisyDriver(DriverDescriptor d, std::string address)
        : descriptor_(std::move(d)), address_(std::move(address)) {}
    const DriverDescriptor& descriptor() const override { return descriptor_; }
    bool                    begin(Transport&) override { return true; }
    ProbeResult             probe() override {
        ProbeResult r;
        if (address_ == "1") {
            r.sawTraffic = true;  // two devices on this id; nothing usable came back
            r.evidence.push_back("replies arrived but failed their checksum");
            return r;
        }
        if (address_ == "2") {
            r.responded       = true;
            r.checksumValid   = true;
            r.confidenceScore = 95;
        }
        return r;
    }
    PollResult           poll(DeviceState&) override { return PollResult::Ok; }
    DeviceIdentity       identity() const override { return {}; }
    InverterCapabilities capabilities() const override { return {}; }
    BusErrorCounts       busErrors() const override { return {}; }
    CommandResult        execute(const InverterCommand&) override { return CommandResult::Ok; }

private:
    DriverDescriptor descriptor_;
    std::string      address_;
};

static void test_an_address_with_traffic_but_no_device_is_reported() {
    DriverRegistry reg;
    MockTransport  t;
    const auto     d = addressedDesc("growatt_like", "1");
    reg.registerDriver(d, [d](Transport&, const DriverOptions& o) {
        return std::make_unique<NoisyDriver>(d, d.optionOr(o, "unit_id"));
    });

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());  // only the clean one at 2
    TEST_ASSERT_EQUAL_size_t(1, out.unidentified.size());
    TEST_ASSERT_EQUAL_STRING("1", out.unidentified[0].address.c_str());
    TEST_ASSERT_TRUE(out.unidentified[0].note.find("checksum") != std::string::npos);
}

// ...and when that is ALL there is, the reason must not send someone to re-crimp a cable that
// is fine. A bus producing traffic is demonstrably wired.
static void test_traffic_without_an_identification_is_not_reported_as_bad_wiring() {
    DriverRegistry reg;
    MockTransport  t;
    const auto     d = addressedDesc("growatt_like", "1");
    reg.registerDriver(d, [d](Transport&, const DriverOptions& o) {
        // Only address 1 exists, and it is the noisy one.
        const std::string addr = d.optionOr(o, "unit_id");
        return std::make_unique<NoisyDriver>(d, addr == "2" ? "9" : addr);
    });

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_TRUE(out.candidates.empty());
    TEST_ASSERT_EQUAL_size_t(1, out.unidentified.size());
    TEST_ASSERT_TRUE(out.reason.find("produced traffic") != std::string::npos);
    TEST_ASSERT_TRUE_MESSAGE(out.reason.find("check wiring") == std::string::npos,
                             "the bus answered; blaming the cable sends someone the wrong way");
}

// One bus, one set of line settings. Once something answers at a profile, the driver's other
// baud rates are silence by construction -- and at eight addresses each, trying them anyway is
// the difference between a slow sweep and an unusable one. Nothing pinned this.
static void test_the_remaining_profiles_are_not_swept_once_one_answers() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied            = {"3"};
    DriverDescriptor d      = addressedDesc("two_speed", "1");
    d.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000},
                                   SerialProfile{115200, SerialParity::None, 8, 1, 1000}};
    addAddressedDriver(reg, d, &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    // Eight addresses at the first profile, and no second pass over them.
    TEST_ASSERT_EQUAL_size_t(8, bus.probed.size());
}

// The driver's default is probed first and is not necessarily the lowest address, so keeping
// "the first one probed" would report a device at the higher of the two while claiming the
// lower. Only reachable for a driver whose default sits above the sweep range.
static void test_a_duplicate_is_kept_at_the_lowest_address_not_the_first_probed() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied     = {"8", "3"};
    bus.sharedSerial = true;
    addAddressedDriver(reg, addressedDesc("high_default", "8"), &bus);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_EQUAL_STRING("3", out.candidates[0].address().c_str());
}

// A driver naming an option that is not numeric, or not declared at all, has nothing a sweep
// can use. Probing it over 1..8 is eight transactions the driver would refuse.
static void test_an_unsweepable_address_option_is_not_swept() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"left"};
    DriverDescriptor enumerated = desc("enum_option", 10);
    enumerated.addressOptionKey = "side";
    enumerated.options = {DriverOption{"side", "Side", "", "left", {"left", "right"}}};
    addAddressedDriver(reg, enumerated, &bus);

    DriverDescriptor undeclared = desc("undeclared_option", 9);
    undeclared.addressOptionKey = "unit_id";  // names an option it never declares
    addAddressedDriver(reg, undeclared, &bus);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Extended);

    TEST_ASSERT_EQUAL_size_t(2, bus.probed.size());  // one probe each, not nine
}

// The watchdog reason for the callback: an extended sweep is dozens of response timeouts inside
// one rs485Task iteration, and the task watchdog does not care that the delay is legitimate.
static void test_every_probe_ticks_the_caller() {
    DriverRegistry reg;
    MockTransport  t;
    AddressBus     bus;
    bus.occupied = {"4"};
    addAddressedDriver(reg, addressedDesc("growatt_like", "1"), &bus);

    int             ticks = 0;
    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Extended, DiscoveryConfig{}, [&ticks] { ++ticks; });

    // Eight addresses, plus the consistency re-probe of the one that answered.
    TEST_ASSERT_EQUAL_INT(9, ticks);
}

// --- the happy path ------------------------------------------------------------------------

static void test_single_convincing_match_is_auto_selected() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("eversolar_like", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("eversolar_like", out.selectedDriverId.c_str());
    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
}

static void test_clear_winner_over_a_weak_match_is_auto_selected() {
    // The example from the brief: 97 vs 35 is not a close call.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script strong;
    strong.score = 97;
    strong.serial = "A";
    FakeDriver::Script weak;
    weak.score = 35;
    weak.serial = "B";
    addDriver(reg, desc("eversolar_like", 10), &strong);
    addDriver(reg, desc("generic_serial", 1), &weak);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("eversolar_like", out.selectedDriverId.c_str());
    TEST_ASSERT_EQUAL_size_t(2, out.candidates.size());
    TEST_ASSERT_EQUAL_INT(97, out.candidates[0].probe.confidenceScore);  // sorted best first
}

// --- when not to decide ---------------------------------------------------------------------

static void test_two_close_candidates_are_never_auto_selected() {
    // The other example from the brief: Growatt MIC 82 vs MIN 78. Guessing here means
    // silently reading the wrong register map, which looks like plausible data.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script a;
    a.score = 82;
    a.serial = "A";
    FakeDriver::Script b;
    b.score = 78;
    b.serial = "B";
    addDriver(reg, desc("growatt_mic", 10), &a);
    addDriver(reg, desc("growatt_min", 10), &b);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_TRUE(out.selectedDriverId.empty());
    TEST_ASSERT_EQUAL_size_t(2, out.candidates.size());
    // The user has to be able to judge it, so both candidates and a reason must be reported.
    TEST_ASSERT_TRUE(out.reason.find("too close") != std::string::npos);
}

static void test_a_score_below_the_threshold_is_never_auto_selected() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 79;  // one point short
    addDriver(reg, desc("almost", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_size_t(1, out.candidates.size());
    TEST_ASSERT_TRUE(out.reason.find("below the threshold") != std::string::npos);
}

static void test_threshold_is_configurable() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 79;
    addDriver(reg, desc("almost", 10), &s);

    DiscoveryConfig c;
    c.minConfidence = 70;
    DiscoveryEngine e(reg, t);
    TEST_ASSERT_TRUE(e.run(DiscoveryMode::Quick, c).autoSelected);
}

static void test_inconsistent_probes_halve_the_score_and_block_selection() {
    // A device that identifies as something different on the second ask was never identified.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score          = 100;
    s.serial         = "SER-1";
    s.serialOnRepeat = "SER-2";
    addDriver(reg, desc("flaky", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_INT(50, out.candidates[0].probe.confidenceScore);
    TEST_ASSERT_FALSE(out.candidates[0].consistent);
}

static void test_consistent_probes_are_recorded_as_evidence() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("solid", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_TRUE(out.candidates[0].consistent);
    TEST_ASSERT_EQUAL_INT(97, out.candidates[0].probe.confidenceScore);  // not halved
}

static void test_a_failed_checksum_blocks_selection() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score         = 97;
    s.checksumValid = false;
    addDriver(reg, desc("noisy", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_TRUE(out.reason.find("checksum") != std::string::npos);
}

static void test_a_silent_bus_yields_no_candidates_and_a_useful_reason() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.responded = false;
    addDriver(reg, desc("absent", 10), &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    TEST_ASSERT_FALSE(out.autoSelected);
    TEST_ASSERT_EQUAL_size_t(0, out.candidates.size());
    TEST_ASSERT_TRUE(out.reason.find("wiring") != std::string::npos);
}

// --- safety --------------------------------------------------------------------------------

static void test_discovery_never_executes_a_command() {
    // The structural guarantee: discovery calls probe() and nothing else, so every forbidden
    // operation is unreachable rather than merely unwritten.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Extended);

    TEST_ASSERT_FALSE(s.writeAttempted);
}

static void test_drivers_opting_out_of_auto_detection_are_skipped() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script sim;
    sim.score = 100;
    addDriver(reg, desc("mock_like", -100, /*autoDetect=*/false), &sim);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Quick);

    // A simulated device scoring 100 must not be discovered on a real bus.
    TEST_ASSERT_EQUAL_size_t(0, out.candidates.size());
    TEST_ASSERT_FALSE(out.autoSelected);
}

static void test_drivers_are_skipped_on_an_unsupported_transport() {
    DriverRegistry     reg;
    MockTransport      t;
    t.setType(TransportType::Tcp);
    FakeDriver::Script s;
    s.score = 97;
    auto d  = desc("serial_only", 10);
    d.supportedTransports = {TransportType::Rs485};
    addDriver(reg, d, &s);

    DiscoveryEngine e(reg, t);
    TEST_ASSERT_EQUAL_size_t(0, e.run(DiscoveryMode::Quick).candidates.size());
}

// The sweep has to survive begin() reconfiguring the line. Every real driver does that, on
// purpose: a driver started straight from config must not depend on a discovery run having set
// up the UART, which was a real bug once. But it silently undid the sweep -- the engine set the
// profile, begin() put it straight back to the driver's own default, and every iteration probed
// at the same rate. A device shipped at the family's other baud rate simply could not be found,
// and the failure looked exactly like bad wiring (review, 2026-07-25).
static void test_a_device_on_the_second_profile_is_still_found() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score              = 97;
    s.begunAtBaud        = 9600;    // what begin() forces, like a real driver
    s.respondsOnlyAtBaud = 115200;  // where the device actually is
    s.bus                = &t;

    auto d = desc("two_rates", 10);
    d.recommendedSerialProfiles = {SerialProfile{9600, SerialParity::None, 8, 1, 1000},
                                   SerialProfile{115200, SerialParity::None, 8, 1, 1000}};
    addDriver(reg, d, &s);

    DiscoveryEngine e(reg, t);
    const auto      out = e.run(DiscoveryMode::Extended);

    TEST_ASSERT_TRUE(out.autoSelected);
    TEST_ASSERT_EQUAL_STRING("two_rates", out.selectedDriverId.c_str());
}

static void test_only_profiles_the_driver_recommends_are_tried() {
    // No brute-forcing baud rates on a live bus.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("one_profile", 10), &s);

    DiscoveryEngine e(reg, t);
    e.run(DiscoveryMode::Quick);

    TEST_ASSERT_EQUAL_UINT32(9600, t.profile().baudRate);
    TEST_ASSERT_TRUE(t.configureCalls <= 2);  // one profile, not a sweep
}

// --- the runner: handing a bus-bound job from the web task to rs485Task -----------------

static void test_a_request_is_not_run_by_the_requester() {
    // The whole point: probing takes the bus for seconds. Doing it inside an AsyncTCP callback
    // would block the web server and race the poll loop.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    TEST_ASSERT_TRUE(runner.request(false));
    TEST_ASSERT_EQUAL(DiscoveryStatus::Requested, runner.report().status);
    TEST_ASSERT_TRUE(runner.busy());
    // Nothing has touched the bus yet.
    TEST_ASSERT_EQUAL_UINT32(0, t.configureCalls);
}

static void test_the_bus_task_picks_the_request_up() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    g_now += 500;
    TEST_ASSERT_TRUE(runner.runIfRequested(t));

    const auto r = runner.report();
    TEST_ASSERT_EQUAL(DiscoveryStatus::Done, r.status);
    TEST_ASSERT_TRUE(r.outcome.autoSelected);
    TEST_ASSERT_EQUAL_STRING("any", r.outcome.selectedDriverId.c_str());
    TEST_ASSERT_FALSE(runner.busy());
}

static void test_running_without_a_request_does_nothing() {
    DriverRegistry  reg;
    MockTransport   t;
    DiscoveryRunner runner(reg, clockFn);
    TEST_ASSERT_FALSE(runner.runIfRequested(t));
    TEST_ASSERT_EQUAL(DiscoveryStatus::Idle, runner.report().status);
}

static void test_a_second_request_is_refused_while_one_is_pending() {
    // The REST layer turns this into 409. Queueing would be worse: a second probe of the same
    // bus tells you nothing the first one will not.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    TEST_ASSERT_TRUE(runner.request(false));
    TEST_ASSERT_FALSE(runner.request(false));
    TEST_ASSERT_FALSE(runner.request(true));

    runner.runIfRequested(t);
    // Once finished, a new run is allowed again.
    TEST_ASSERT_TRUE(runner.request(false));
}

static void test_a_new_run_discards_the_previous_result() {
    // A report half old and half new is worse than either: the bus may have changed.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    runner.runIfRequested(t);
    TEST_ASSERT_EQUAL_size_t(1, runner.report().outcome.candidates.size());

    runner.request(false);
    TEST_ASSERT_EQUAL_size_t(0, runner.report().outcome.candidates.size());
    TEST_ASSERT_EQUAL(DiscoveryStatus::Requested, runner.report().status);
}

static void test_extended_mode_is_carried_through() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(true);
    TEST_ASSERT_EQUAL(DiscoveryMode::Extended, runner.report().mode);
    runner.request(false);  // refused while pending, so the mode must not change
    TEST_ASSERT_EQUAL(DiscoveryMode::Extended, runner.report().mode);
}

static void test_timings_are_recorded_for_the_wizard() {
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(false);
    g_now += 200;
    runner.runIfRequested(t);

    const auto r = runner.report();
    TEST_ASSERT_EQUAL_UINT64(1000, r.requestedMs);
    TEST_ASSERT_EQUAL_UINT64(1200, r.startedMs);
    TEST_ASSERT_TRUE(r.finishedMs >= r.startedMs);
}

static void test_the_runner_never_executes_a_command() {
    // Same structural guarantee as the engine, now through the app layer.
    DriverRegistry     reg;
    MockTransport      t;
    FakeDriver::Script s;
    s.score = 97;
    addDriver(reg, desc("any", 10), &s);
    DiscoveryRunner runner(reg, clockFn);

    runner.request(true);
    runner.runIfRequested(t);
    TEST_ASSERT_FALSE(s.writeAttempted);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_request_is_not_run_by_the_requester);
    RUN_TEST(test_the_bus_task_picks_the_request_up);
    RUN_TEST(test_running_without_a_request_does_nothing);
    RUN_TEST(test_a_second_request_is_refused_while_one_is_pending);
    RUN_TEST(test_a_new_run_discards_the_previous_result);
    RUN_TEST(test_extended_mode_is_carried_through);
    RUN_TEST(test_timings_are_recorded_for_the_wizard);
    RUN_TEST(test_the_runner_never_executes_a_command);
    RUN_TEST(test_extended_discovery_finds_devices_at_other_addresses);
    RUN_TEST(test_quick_discovery_probes_the_default_address_and_claims_nothing);
    RUN_TEST(test_the_drivers_own_default_is_probed_even_outside_the_sweep);
    RUN_TEST(test_the_sweep_stays_inside_the_declared_bounds);
    RUN_TEST(test_a_driver_without_an_address_option_is_probed_once);
    RUN_TEST(test_identical_inverters_do_not_block_auto_selection);
    RUN_TEST(test_one_device_answering_at_two_addresses_is_reported_once);
    RUN_TEST(test_an_address_with_traffic_but_no_device_is_reported);
    RUN_TEST(test_traffic_without_an_identification_is_not_reported_as_bad_wiring);
    RUN_TEST(test_the_remaining_profiles_are_not_swept_once_one_answers);
    RUN_TEST(test_a_duplicate_is_kept_at_the_lowest_address_not_the_first_probed);
    RUN_TEST(test_an_unsweepable_address_option_is_not_swept);
    RUN_TEST(test_every_probe_ticks_the_caller);
    RUN_TEST(test_single_convincing_match_is_auto_selected);
    RUN_TEST(test_clear_winner_over_a_weak_match_is_auto_selected);
    RUN_TEST(test_two_close_candidates_are_never_auto_selected);
    RUN_TEST(test_a_score_below_the_threshold_is_never_auto_selected);
    RUN_TEST(test_threshold_is_configurable);
    RUN_TEST(test_inconsistent_probes_halve_the_score_and_block_selection);
    RUN_TEST(test_consistent_probes_are_recorded_as_evidence);
    RUN_TEST(test_a_failed_checksum_blocks_selection);
    RUN_TEST(test_a_silent_bus_yields_no_candidates_and_a_useful_reason);
    RUN_TEST(test_discovery_never_executes_a_command);
    RUN_TEST(test_drivers_opting_out_of_auto_detection_are_skipped);
    RUN_TEST(test_drivers_are_skipped_on_an_unsupported_transport);
    RUN_TEST(test_a_device_on_the_second_profile_is_still_found);
    RUN_TEST(test_only_profiles_the_driver_recommends_are_tried);
    return UNITY_END();
}
