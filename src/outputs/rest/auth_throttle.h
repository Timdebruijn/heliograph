// SPDX-License-Identifier: MIT
//
// Slows down guessing the admin password.
//
// The admin password is the only thing between a LAN host and every mutating route: settings,
// OTA, factory reset, and with control allowed the relays and inverter setpoints. Nothing
// counted failed attempts, so a script could try passwords as fast as the ESP32 answers, and the
// open setup AP the bridge raises after a WiFi outage put that within radio range too.
//
// Two limits, both in RAM (a reboot clears them):
//
//  - Per source address. After `freeAttempts` wrong passwords in a row, that address is locked
//    out: 30 s, doubling with every further failure, capped at 15 minutes. While locked out even
//    the RIGHT password is refused, or the lockout would only slow a guesser down by the time it
//    takes to be told "wrong". A correct password outside a lockout clears the address. Per
//    address so that one machine's typos, or a stale script, do not lock out every other.
//
//  - Across all addresses. A per-address table alone is defeated by a host with more addresses
//    than slots (17 aliases on one NIC, or anyone picking static IPs on the open setup AP):
//    evicting a locked entry hands it fresh attempts. So at most `globalMaxFailures` failures are
//    accepted per `globalWindowMs` from everyone together; past that, every password check is
//    refused for `globalLockoutMs`. That caps guessing at roughly 20 a minute however many
//    addresses are used. Typos never get there -- one address locks at five.
//
//    Except for an address that has signed in with the right password since boot (the last
//    kTrusted of them): the global limit does not apply to it, its own per-address limit still
//    does. Without that exemption any LAN host with ~20 addresses could keep the owner out
//    indefinitely at about 20 requests a minute. An attacker cannot earn the exemption without
//    the password; one that shares the owner's address (a proxy, NAT) gets that address's own
//    five tries and doubling, which the global limit stops others from resetting -- a globally
//    refused address cannot fail, so it cannot evict anything. After a reboot the list is empty
//    and the owner is subject to the global limit until they have signed in once.
//
// "Address" is what the TCP connection comes from: behind a reverse proxy or NAT that is the
// proxy, shared by everyone behind it.
//
// Not thread-safe: every REST handler runs on the one AsyncTCP task. Header-only and pure, so
// the native suite tests it with an injected clock.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace heliograph::rest {

struct AuthThrottlePolicy {
    uint32_t freeAttempts      = 5;
    uint64_t baseLockoutMs     = 30ULL * 1000;
    uint64_t maxLockoutMs      = 15ULL * 60 * 1000;
    uint32_t globalMaxFailures = 20;
    uint64_t globalWindowMs    = 60ULL * 1000;
    uint64_t globalLockoutMs   = 60ULL * 1000;
};

enum class AuthVerdict : uint8_t {
    Ok,            ///< right credentials
    NoCredential,  ///< none sent: a browser asking for the prompt; not counted
    Wrong,         ///< sent and wrong; counted
    Locked,        ///< not checked at all: this address, or everyone, is locked out
};

class AuthThrottle {
public:
    static constexpr std::size_t kSlots   = 16;
    static constexpr std::size_t kTrusted = 4;

    AuthThrottle() = default;
    explicit AuthThrottle(AuthThrottlePolicy policy) : policy_(policy) {}

    /// The whole decision, in the order that matters: a locked-out client's password is never
    /// looked at, so `credentialsOk` is not even called. `sentCredential` is whether the request
    /// carried an Authorization header at all.
    template <typename CredentialsOk>
    AuthVerdict check(const std::string& client, uint64_t nowMs, bool sentCredential,
                      CredentialsOk&& credentialsOk) {
        if (retryAfterMs(client, nowMs) > 0) {
            return AuthVerdict::Locked;
        }
        if (credentialsOk()) {
            recordSuccess(client);
            return AuthVerdict::Ok;
        }
        if (!sentCredential) {
            return AuthVerdict::NoCredential;
        }
        recordFailure(client, nowMs);
        return AuthVerdict::Wrong;
    }

    /// Milliseconds until `client` may try a password again; 0 means it may now.
    uint64_t retryAfterMs(const std::string& client, uint64_t nowMs) const {
        const std::string key  = keyFor(client);
        uint64_t          wait = 0;
        if (nowMs < globalLockedUntilMs_ && !isTrusted(key)) {
            wait = globalLockedUntilMs_ - nowMs;
        }
        if (const Entry* e = find(key); e != nullptr && nowMs < e->lockedUntilMs) {
            const uint64_t own = e->lockedUntilMs - nowMs;
            wait               = own > wait ? own : wait;
        }
        return wait;
    }

    /// A wrong password from `client`.
    void recordFailure(const std::string& client, uint64_t nowMs) {
        recordGlobalFailure(nowMs);
        const std::string key = keyFor(client);
        Entry*            e   = find(key);
        if (e == nullptr) {
            e      = &slotToReuse(nowMs);
            *e     = Entry{};
            e->key = key;
        } else if (nowMs > quietSinceMs(*e) && nowMs - quietSinceMs(*e) > policy_.maxLockoutMs) {
            // Quiet for longer than the longest lockout AFTER the last one ended: an old run of
            // typos, not a guesser that is still at it. Measured from the end of the lockout,
            // or a guesser at the cap would earn fresh attempts just by waiting it out.
            e->failures = 0;
        }
        e->lastFailureMs = nowMs;
        if (e->failures < UINT32_MAX) {
            ++e->failures;
        }
        if (e->failures >= policy_.freeAttempts) {
            e->lockedUntilMs = nowMs + lockoutFor(e->failures - policy_.freeAttempts);
        }
    }

    /// The right password from `client`, outside a lockout.
    void recordSuccess(const std::string& client) {
        const std::string key = keyFor(client);
        if (Entry* e = find(key)) {
            *e = Entry{};
        }
        trust(key);
    }

private:
    struct Entry {
        std::string key;  // empty = free slot
        uint32_t    failures      = 0;
        uint64_t    lastFailureMs = 0;
        uint64_t    lockedUntilMs = 0;
    };

    /// An address the server could not name still gets a bucket of its own, rather than an
    /// empty key that reads as a free slot and is never throttled.
    static std::string keyFor(const std::string& client) { return client.empty() ? "?" : client; }

    bool isTrusted(const std::string& key) const {
        for (const auto& t : trusted_) {
            if (!t.empty() && t == key) {
                return true;
            }
        }
        return false;
    }

    /// Most recent first; the oldest falls off the end.
    void trust(const std::string& key) {
        std::size_t at = kTrusted - 1;  // not present: shift everything, drop the last
        for (std::size_t i = 0; i < kTrusted; ++i) {
            if (trusted_[i] == key) {
                at = i;
                break;
            }
        }
        for (std::size_t i = at; i > 0; --i) {
            trusted_[i] = std::move(trusted_[i - 1]);
        }
        trusted_[0] = key;
    }

    static uint64_t quietSinceMs(const Entry& e) {
        return e.lockedUntilMs > e.lastFailureMs ? e.lockedUntilMs : e.lastFailureMs;
    }

    void recordGlobalFailure(uint64_t nowMs) {
        if (!globalWindowOpen_ || nowMs < globalWindowStartMs_ ||
            nowMs - globalWindowStartMs_ >= policy_.globalWindowMs) {
            globalWindowOpen_    = true;
            globalWindowStartMs_ = nowMs;
            globalFailures_      = 0;
        }
        if (++globalFailures_ >= policy_.globalMaxFailures) {
            globalLockedUntilMs_ = nowMs + policy_.globalLockoutMs;
            globalWindowOpen_    = false;  // a fresh window once the lockout ends
        }
    }

    /// 30 s, 60 s, 120 s, ... capped. `extra` is the number of failures past the free ones.
    uint64_t lockoutFor(uint32_t extra) const {
        uint64_t ms = policy_.baseLockoutMs;
        for (uint32_t i = 0; i < extra && ms < policy_.maxLockoutMs; ++i) {
            ms *= 2;
        }
        return ms < policy_.maxLockoutMs ? ms : policy_.maxLockoutMs;
    }

    const Entry* find(const std::string& key) const {
        for (const auto& e : entries_) {
            if (!e.key.empty() && e.key == key) {
                return &e;
            }
        }
        return nullptr;
    }
    Entry* find(const std::string& key) {
        return const_cast<Entry*>(static_cast<const AuthThrottle*>(this)->find(key));
    }

    /// A free slot; else the unlocked entry seen longest ago; else, only when every slot is
    /// locked, the oldest locked one. Evicting a locked entry gives it fresh attempts, which is
    /// why the global limit exists.
    Entry& slotToReuse(uint64_t nowMs) {
        Entry* oldestUnlocked = nullptr;
        Entry* oldest         = &entries_[0];
        for (auto& e : entries_) {
            if (e.key.empty()) {
                return e;
            }
            if (e.lastFailureMs < oldest->lastFailureMs) {
                oldest = &e;
            }
            if (nowMs >= e.lockedUntilMs &&
                (oldestUnlocked == nullptr || e.lastFailureMs < oldestUnlocked->lastFailureMs)) {
                oldestUnlocked = &e;
            }
        }
        return oldestUnlocked != nullptr ? *oldestUnlocked : *oldest;
    }

    AuthThrottlePolicy        policy_{};
    std::array<Entry, kSlots> entries_{};
    std::array<std::string, kTrusted> trusted_{};
    bool                      globalWindowOpen_    = false;
    uint64_t                  globalWindowStartMs_ = 0;
    uint32_t                  globalFailures_      = 0;
    uint64_t                  globalLockedUntilMs_ = 0;
};

}  // namespace heliograph::rest
