// SPDX-License-Identifier: MIT
// The shared throttle, and the two traps its arithmetic exists to avoid.
//
// Both users -- CommandDispatcher for inverter writes, RelayController for DRM contacts --
// already cover it through their own gate tests. This suite exists because those two prove it
// twice from the outside while neither states the invariants directly, and because both traps
// below were found on hardware in one copy and only later fixed in the other. Now there is one
// copy, and this is where it says what it guarantees.

#include <unity.h>

#include "rate_limiter.h"

using heliograph::RateLimiter;
using heliograph::RateLimitPolicy;

void setUp() {}
void tearDown() {}

// --- the ordinary contract ------------------------------------------------------------------

static void test_the_burst_is_spent_then_the_spacing_applies() {
    RateLimiter limiter{RateLimitPolicy{1000, 3}};
    TEST_ASSERT_TRUE(limiter.allow(10000));
    TEST_ASSERT_TRUE(limiter.allow(10001));
    TEST_ASSERT_TRUE(limiter.allow(10002));
    TEST_ASSERT_FALSE(limiter.allow(10003));  // burst gone, spacing not yet met
}

static void test_a_quiet_period_refills_the_whole_allowance() {
    RateLimiter limiter{RateLimitPolicy{1000, 2}};
    TEST_ASSERT_TRUE(limiter.allow(5000));
    TEST_ASSERT_TRUE(limiter.allow(5001));
    TEST_ASSERT_FALSE(limiter.allow(5002));
    TEST_ASSERT_TRUE(limiter.allow(6002));   // exactly minIntervalMs later
    TEST_ASSERT_TRUE(limiter.allow(6003));   // and the burst is back
}

/// A refusal must not move the clock forward, or a caller that retries in a tight loop would
/// push its own next opportunity out indefinitely -- the throttle would become a lockout.
static void test_a_refusal_does_not_consume_the_allowance() {
    RateLimiter limiter{RateLimitPolicy{1000, 1}};
    TEST_ASSERT_TRUE(limiter.allow(1000));
    for (uint64_t t = 1001; t < 1999; ++t) {
        TEST_ASSERT_FALSE(limiter.allow(t));
    }
    TEST_ASSERT_TRUE(limiter.allow(2000));  // still exactly one interval after the ACCEPTED one
}

// --- trap 1: the boot-time sentinel -----------------------------------------------------------

/// millis() at boot IS near zero. A limiter that used lastAcceptedMs_ == 0 to mean "never
/// accepted" would read t=0 as "accepted at time 0, ages ago" and let an unbounded burst
/// through in the first seconds after a restart -- exactly when something is retrying.
static void test_time_zero_is_not_mistaken_for_never_accepted() {
    RateLimiter limiter{RateLimitPolicy{1000, 1}};
    TEST_ASSERT_TRUE(limiter.allow(0));
    TEST_ASSERT_FALSE(limiter.allow(0));
    TEST_ASSERT_FALSE(limiter.allow(1));
    TEST_ASSERT_FALSE(limiter.allow(999));
    TEST_ASSERT_TRUE(limiter.allow(1000));
}

// --- trap 2: the clock that steps backwards ---------------------------------------------------

/// Elapsed time is clamped, not wrapped. On an unsigned type a backwards clock turns the
/// subtraction into an enormous number, which reads as "quiet for ages" and refills everything:
/// the throttle fails OPEN, the permissive direction. The shipping clock is monotonic, but it
/// is injectable, so this is the behaviour under an injected one.
static void test_a_backwards_clock_does_not_refill_the_allowance() {
    RateLimiter limiter{RateLimitPolicy{1000, 1}};
    TEST_ASSERT_TRUE(limiter.allow(100000));
    TEST_ASSERT_FALSE(limiter.allow(100001));
    TEST_ASSERT_FALSE(limiter.allow(50000));  // clock jumped back; must NOT be treated as ages
    TEST_ASSERT_FALSE(limiter.allow(1));
}

// --- the degenerate policies ------------------------------------------------------------------

/// burst 0 means "spacing only, no allowance". The dispatcher's release track has exactly this
/// shape and deliberately keeps its own fields rather than relying on this equivalence -- but
/// the equivalence had better be real, or a future caller that does use it gets a surprise.
static void test_a_zero_burst_leaves_only_the_spacing() {
    RateLimiter limiter{RateLimitPolicy{1000, 0}};
    TEST_ASSERT_TRUE(limiter.allow(500));   // first call: never accepted before
    TEST_ASSERT_FALSE(limiter.allow(501));
    TEST_ASSERT_TRUE(limiter.allow(1500));
}

/// A zero interval is "burst only, refilled every call", not a division by anything.
static void test_a_zero_interval_never_refuses() {
    RateLimiter limiter{RateLimitPolicy{0, 1}};
    for (uint64_t t = 0; t < 100; ++t) {
        TEST_ASSERT_TRUE(limiter.allow(t));
    }
}

/// Default-constructed and policy-constructed must agree, since RelayController default-
/// constructs its member and assigns the policy through the constructor initialiser list.
static void test_the_default_policy_is_one_second_and_three() {
    RateLimiter limiter;
    TEST_ASSERT_TRUE(limiter.allow(10000));
    TEST_ASSERT_TRUE(limiter.allow(10000));
    TEST_ASSERT_TRUE(limiter.allow(10000));
    TEST_ASSERT_FALSE(limiter.allow(10000));
    TEST_ASSERT_TRUE(limiter.allow(11000));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_burst_is_spent_then_the_spacing_applies);
    RUN_TEST(test_a_quiet_period_refills_the_whole_allowance);
    RUN_TEST(test_a_refusal_does_not_consume_the_allowance);
    RUN_TEST(test_time_zero_is_not_mistaken_for_never_accepted);
    RUN_TEST(test_a_backwards_clock_does_not_refill_the_allowance);
    RUN_TEST(test_a_zero_burst_leaves_only_the_spacing);
    RUN_TEST(test_a_zero_interval_never_refuses);
    RUN_TEST(test_the_default_policy_is_one_second_and_three);
    return UNITY_END();
}
