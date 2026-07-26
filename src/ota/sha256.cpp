// SPDX-License-Identifier: MIT

#include "sha256.h"

#include <cstring>

namespace heliograph::ota {
namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the
// first 64 primes. Constants, not choices -- there is nothing here to tune.
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t be32At(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

void Sha256::reset() {
    // Fractional parts of the square roots of the first eight primes.
    state_[0]  = 0x6a09e667;
    state_[1]  = 0xbb67ae85;
    state_[2]  = 0x3c6ef372;
    state_[3]  = 0xa54ff53a;
    state_[4]  = 0x510e527f;
    state_[5]  = 0x9b05688c;
    state_[6]  = 0x1f83d9ab;
    state_[7]  = 0x5be0cd19;
    totalBits_ = 0;
    buffered_  = 0;
}

void Sha256::compress(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = be32At(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch    = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (data == nullptr) {
        return;
    }
    totalBits_ += static_cast<uint64_t>(len) * 8u;
    // Finish a partial block first, then run whole blocks straight out of the caller's buffer.
    // The OTA path hands this megabyte-sized images in whatever chunks the TCP stack produced,
    // so neither a copy per byte nor an assumption of 64-byte alignment would do.
    while (len > 0) {
        if (buffered_ == 0 && len >= 64) {
            compress(data);
            data += 64;
            len -= 64;
            continue;
        }
        const size_t take = (64 - buffered_) < len ? (64 - buffered_) : len;
        std::memcpy(buffer_ + buffered_, data, take);
        buffered_ += take;
        data += take;
        len -= take;
        if (buffered_ == 64) {
            compress(buffer_);
            buffered_ = 0;
        }
    }
}

void Sha256::finish(uint8_t out[32]) {
    // Padding: a 1 bit, zeroes, then the 64-bit big-endian length. When the length will not fit
    // in the current block, one more block is emitted -- the case a naive implementation gets
    // wrong for messages of 56..63 bytes mod 64, which is why the tests include one.
    const uint64_t bits    = totalBits_;
    uint8_t        padByte = 0x80;
    update(&padByte, 1);
    const uint8_t zero = 0x00;
    while (buffered_ != 56) {
        update(&zero, 1);
    }
    // Written directly rather than through update(), which would count these eight bytes into
    // a length that has already been captured.
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xFF);
    }
    std::memcpy(buffer_ + 56, lengthBytes, 8);
    compress(buffer_);
    buffered_ = 0;

    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
    }
}

std::string Sha256::finishHex() {
    uint8_t digest[32];
    finish(digest);
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(64);
    for (unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

bool hexDigestEquals(const std::string& expectedHex, const uint8_t digest[32]) {
    if (expectedHex.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        const int hi = hexValue(expectedHex[i * 2]);
        const int lo = hexValue(expectedHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;  // malformed is refused, never compared
        }
        if (static_cast<uint8_t>((hi << 4) | lo) != digest[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace heliograph::ota
