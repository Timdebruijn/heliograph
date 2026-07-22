// SPDX-License-Identifier: MIT
//
// RelayController: the gate order and the failsafe, proven on the host before any coil
// ever clicks. These tests are the contract that a relay board with default settings is
// inert, and that a dead or read-only bridge can never assert curtailment.

#include <cstdint>
#include <vector>

#include <unity.h>

#include "relays/relay_controller.h"

using namespace heliograph;

namespace {

uint64_t g_now = 0;
uint64_t fakeClock() { return g_now; }

struct AppliedWrite {
    uint8_t index;
    bool    energised;
};
std::vector<AppliedWrite> g_writes;

RelayController makeController(uint8_t count) {
    RelayController c(&fakeClock);
    c.begin(count, [](uint8_t i, bool on) { g_writes.push_back({i, on}); });
    return c;
}

}  // namespace

void setUp() {
    g_now = 0;
    g_writes.clear();
}
void tearDown() {}

static void test_boot_drives_every_relay_off() {
    auto c = makeController(6);
    TEST_ASSERT_EQUAL_UINT32(6, g_writes.size());
    for (const auto& w : g_writes) {
        TEST_ASSERT_FALSE(w.energised);
    }
    for (uint8_t i = 0; i < 6; ++i) {
        TEST_ASSERT_FALSE(c.energised(i));
    }
}

static void test_default_state_refuses_everything() {
    // Fresh controller: read-only ON, enabled OFF. The kill switch answers first.
    auto c = makeController(1);
    g_writes.clear();
    TEST_ASSERT_EQUAL(CommandResult::ReadOnlyMode, c.set(0, true));
    TEST_ASSERT_TRUE(g_writes.empty());
    TEST_ASSERT_FALSE(c.energised(0));
}

static void test_read_only_wins_over_enabled() {
    auto c = makeController(1);
    c.setEnabled(true);  // enabled, but the kill switch still holds
    TEST_ASSERT_EQUAL(CommandResult::ReadOnlyMode, c.set(0, true));
}

static void test_disabled_feature_refuses_even_when_writable() {
    auto c = makeController(1);
    c.setReadOnlyMode(false);
    TEST_ASSERT_EQUAL(CommandResult::Rejected, c.set(0, true));
    TEST_ASSERT_FALSE(c.energised(0));
}

static void test_switching_works_when_both_gates_open() {
    auto c = makeController(2);
    c.setReadOnlyMode(false);
    c.setEnabled(true);
    g_writes.clear();
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.set(1, true));
    TEST_ASSERT_TRUE(c.energised(1));
    TEST_ASSERT_FALSE(c.energised(0));
    TEST_ASSERT_EQUAL_UINT32(1, g_writes.size());
    TEST_ASSERT_EQUAL_UINT8(1, g_writes[0].index);
    TEST_ASSERT_TRUE(g_writes[0].energised);
}

static void test_out_of_range_index_is_refused() {
    auto c = makeController(1);
    c.setReadOnlyMode(false);
    c.setEnabled(true);
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, c.set(1, true));
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, c.set(255, true));
}

static void test_rate_limit_throttles_on_but_never_off() {
    RateLimitPolicy policy;
    policy.minIntervalMs = 1000;
    policy.burst         = 2;
    RelayController c(&fakeClock, policy);
    c.begin(1, [](uint8_t i, bool on) { g_writes.push_back({i, on}); });
    c.setReadOnlyMode(false);
    c.setEnabled(true);

    TEST_ASSERT_EQUAL(CommandResult::Ok, c.set(0, true));
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.set(0, true));
    // Burst exhausted, no time passed: asserting again is throttled...
    TEST_ASSERT_EQUAL(CommandResult::RateLimited, c.set(0, true));
    // ...but releasing curtailment must never wait behind the throttle.
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.set(0, false));
    TEST_ASSERT_FALSE(c.energised(0));
    // After the interval the allowance refills.
    g_now += 1000;
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.set(0, true));
}

static void test_pattern_wider_than_burst_asserts_in_one_command() {
    // The bug this guards against: charging the rate limiter per relay made a role spanning
    // more relays than the burst (default 3) impossible to assert -- the 4th ON was always
    // throttled and the rollback released the whole mode again.
    auto c = makeController(6);
    c.setReadOnlyMode(false);
    c.setEnabled(true);
    g_writes.clear();
    TEST_ASSERT_EQUAL(CommandResult::Ok,
                      c.applyPattern({true, true, true, true, false, false}));
    for (uint8_t i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(c.energised(i));
    }
    TEST_ASSERT_FALSE(c.energised(4));
    TEST_ASSERT_FALSE(c.energised(5));
    // OFFs are written before ONs: releasing the old role precedes asserting the new one.
    TEST_ASSERT_EQUAL_UINT32(6, g_writes.size());
    TEST_ASSERT_FALSE(g_writes[0].energised);
    TEST_ASSERT_FALSE(g_writes[1].energised);
    for (size_t i = 2; i < 6; ++i) {
        TEST_ASSERT_TRUE(g_writes[i].energised);
    }
}

static void test_pattern_charges_one_token_and_throttles_as_a_whole() {
    RateLimitPolicy policy;
    policy.minIntervalMs = 1000;
    policy.burst         = 1;
    RelayController c(&fakeClock, policy);
    c.begin(2, [](uint8_t i, bool on) { g_writes.push_back({i, on}); });
    c.setReadOnlyMode(false);
    c.setEnabled(true);

    TEST_ASSERT_EQUAL(CommandResult::Ok, c.applyPattern({true, false}));
    // Burst spent, no time passed: a second asserting pattern is refused BEFORE any relay
    // moves -- no half-applied mode, no sneaky release of the current one.
    g_writes.clear();
    TEST_ASSERT_EQUAL(CommandResult::RateLimited, c.applyPattern({false, true}));
    TEST_ASSERT_TRUE(g_writes.empty());
    TEST_ASSERT_TRUE(c.energised(0));
    TEST_ASSERT_FALSE(c.energised(1));
    // An all-off pattern is the safe direction and passes the throttle unconditionally.
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.applyPattern({false, false}));
    TEST_ASSERT_FALSE(c.energised(0));
    // After the interval the allowance refills.
    g_now += 1000;
    TEST_ASSERT_EQUAL(CommandResult::Ok, c.applyPattern({false, true}));
    TEST_ASSERT_TRUE(c.energised(1));
}

static void test_pattern_honours_gates_and_size() {
    auto c = makeController(2);
    // Kill switch first, exactly like set().
    TEST_ASSERT_EQUAL(CommandResult::ReadOnlyMode, c.applyPattern({true, false}));
    c.setReadOnlyMode(false);
    TEST_ASSERT_EQUAL(CommandResult::Rejected, c.applyPattern({true, false}));
    c.setEnabled(true);
    // A pattern that does not match the relay count is a caller bug, not a partial apply.
    g_writes.clear();
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, c.applyPattern({true}));
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, c.applyPattern({true, false, true}));
    TEST_ASSERT_TRUE(g_writes.empty());
}

static void test_all_off_ignores_every_gate() {
    auto c = makeController(3);
    c.setReadOnlyMode(false);
    c.setEnabled(true);
    c.set(0, true);
    c.set(2, true);
    // Feature gets disabled at runtime (config change): the failsafe path must still work
    // with the gates closed again.
    c.setEnabled(false);
    c.setReadOnlyMode(true);
    g_writes.clear();
    c.allOff();
    TEST_ASSERT_FALSE(c.energised(0));
    TEST_ASSERT_FALSE(c.energised(2));
    TEST_ASSERT_EQUAL_UINT32(3, g_writes.size());
}

static void test_count_is_clamped_and_zero_count_is_inert() {
    auto c = makeController(0);
    c.setReadOnlyMode(false);
    c.setEnabled(true);
    TEST_ASSERT_EQUAL_UINT8(0, c.count());
    TEST_ASSERT_EQUAL(CommandResult::OutOfRange, c.set(0, true));

    auto big = makeController(200);
    TEST_ASSERT_EQUAL_UINT8(RelayController::kMaxRelays, big.count());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_drives_every_relay_off);
    RUN_TEST(test_default_state_refuses_everything);
    RUN_TEST(test_read_only_wins_over_enabled);
    RUN_TEST(test_disabled_feature_refuses_even_when_writable);
    RUN_TEST(test_switching_works_when_both_gates_open);
    RUN_TEST(test_out_of_range_index_is_refused);
    RUN_TEST(test_rate_limit_throttles_on_but_never_off);
    RUN_TEST(test_pattern_wider_than_burst_asserts_in_one_command);
    RUN_TEST(test_pattern_charges_one_token_and_throttles_as_a_whole);
    RUN_TEST(test_pattern_honours_gates_and_size);
    RUN_TEST(test_all_off_ignores_every_gate);
    RUN_TEST(test_count_is_clamped_and_zero_count_is_inert);
    return UNITY_END();
}
