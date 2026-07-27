// SPDX-License-Identifier: MIT
//
// IPv4 text and subnet arithmetic, with no network stack behind it.
//
// Exists as its own module because static-IP configuration is validated in config/ (where the
// settings are checked before they are stored) and applied in network/ (where they reach the
// WiFi driver), and both need exactly the same notion of what a valid address is. Pure, so the
// rules that decide whether a bridge will still be reachable after a reboot are host-tested
// rather than discovered on a roof.
//
// STRICT ON PURPOSE. inet_aton() and friends accept "10.1" as 10.0.0.1, treat a leading zero as
// octal, and shrug at trailing text. Every one of those is a way for an operator to type
// something that reads like the address they meant and configures a different one -- and the
// symptom is a bridge that never comes back. Only four decimal octets, nothing else.

#pragma once

#include <cstdint>
#include <string>

namespace heliograph::net {

/// Parses dotted-quad IPv4 into host byte order. False on anything that is not exactly four
/// decimal octets in 0..255: no shorthand, no leading zeros, no whitespace, no trailing text.
bool parseIpv4(const std::string& text, uint32_t& out);

std::string formatIpv4(uint32_t addr);

/// True for a contiguous netmask -- a run of ones followed by a run of zeros. 255.255.255.0 and
/// 255.255.254.0 yes; 255.0.255.0 no. A non-contiguous mask is not a mask anyone means to type,
/// and lwip's behaviour with one is not something to find out in production.
bool isContiguousMask(uint32_t mask);

/// Prefix length of a contiguous mask (255.255.255.0 -> 24). Undefined for a non-contiguous one,
/// so check first.
uint8_t maskPrefixLength(uint32_t mask);

bool sameSubnet(uint32_t a, uint32_t b, uint32_t mask);

/// The all-zero host part: names the network, never a host.
bool isNetworkAddress(uint32_t addr, uint32_t mask);
/// The all-ones host part: the directed broadcast, never a host.
bool isBroadcastAddress(uint32_t addr, uint32_t mask);

/// Whether a configured host/server string is a literal address rather than a name.
///
/// Used for the rule that a static configuration with no DNS server must not be accepted while
/// something is still configured by name -- see validate(). Deliberately the same strict parse:
/// a string this rejects is treated as a NAME, which is the conservative direction (it demands
/// a resolver rather than assuming none is needed).
bool looksLikeIpLiteral(const std::string& text);

}  // namespace heliograph::net
