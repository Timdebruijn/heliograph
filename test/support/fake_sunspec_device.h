// SPDX-License-Identifier: MIT
//
// A SunSpec device on a simulated Modbus bus: marker, a chain of model blocks, and whatever
// register values the test wants in them.
//
// Built to be WRONG in the specific ways real devices are wrong, because that is what the
// driver has to survive: a chain with no terminator, a chain that never ends, a device sitting
// at a non-standard base address, and one that answers the marker but carries no model this
// driver can read.

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "drivers/sunspec/sunspec_parser.h"
#include "protocols/modbus/modbus_rtu.h"

namespace heliograph::test {

class FakeSunspecDevice {
public:
    uint8_t  unitId      = 1;
    uint16_t baseAddress = 40000;

    /// When false the chain simply stops answering instead of serving 0xFFFF -- several real
    /// devices behave exactly like this, and it must not read as a failure.
    bool serveTerminator = true;

    /// Registers this device will answer for. Anything outside earns an illegal-data-address
    /// exception, like a real slave.
    std::map<uint16_t, uint16_t> registers;

    /// Lays down the marker and returns the address the first model block should start at.
    uint16_t placeMarker() {
        registers[baseAddress]     = sunspec::kMarkerHigh;
        registers[baseAddress + 1] = sunspec::kMarkerLow;
        return static_cast<uint16_t>(baseAddress + 2);
    }

    /// Appends a block at `at` and returns the address just past it. `payload` excludes the
    /// two header registers.
    uint16_t addModel(uint16_t at, uint16_t modelId, const std::vector<uint16_t>& payload) {
        registers[at]     = modelId;
        registers[at + 1] = static_cast<uint16_t>(payload.size());
        for (size_t i = 0; i < payload.size(); ++i) {
            registers[static_cast<uint16_t>(at + 2 + i)] = payload[i];
        }
        return static_cast<uint16_t>(at + 2 + payload.size());
    }

    void terminate(uint16_t at) {
        if (serveTerminator) {
            registers[at] = sunspec::kEndOfChain;
        }
    }

    /// A model 103 payload with everything not-implemented, ready to have points filled in.
    /// Indices are the OFFSETS FROM THE MODEL ID, so callers can use the parser's constants
    /// directly and a mismatch between fixture and parser cannot hide.
    static std::vector<uint16_t> blankInverterPayload() {
        std::vector<uint16_t> block(sunspec::inverter::kMinRegisters + 1,
                                    sunspec::kNotImplementedU16);
        block[sunspec::inverter::kW]      = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kDCW]    = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kTmpCab] = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kA_SF]   = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kV_SF]   = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kW_SF]   = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kHz_SF]  = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kWH_SF]  = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kDCW_SF] = sunspec::kNotImplementedS16;
        block[sunspec::inverter::kTmp_SF] = sunspec::kNotImplementedS16;
        return block;
    }

    /// A model 123 block with every point at its not-implemented sentinel, for a test to fill
    /// in only what it is about. Indices are offsets from the model id, like the inverter block.
    static std::vector<uint16_t> blankControlsPayload() {
        std::vector<uint16_t> block(sunspec::controls::kMinRegisters, sunspec::kNotImplementedU16);
        block[sunspec::controls::kWMaxLimPct_SF] = sunspec::kNotImplementedS16;
        return block;
    }

    /// Strips the two header registers off a block built with the offsets above, which is what
    /// addModel() wants.
    static std::vector<uint16_t> asPayload(const std::vector<uint16_t>& block) {
        return std::vector<uint16_t>(block.begin() + 2, block.end());
    }

    /// A common model (1) payload carrying the given identity strings.
    static std::vector<uint16_t> commonPayload(const std::string& manufacturer,
                                               const std::string& model,
                                               const std::string& serial) {
        std::vector<uint16_t> block(sunspec::common::kMinRegisters, 0);
        writeString(block, sunspec::common::kMn, manufacturer, 16);
        writeString(block, sunspec::common::kMd, model, 16);
        writeString(block, sunspec::common::kSN, serial, 16);
        return asPayload(block);
    }

    /// The MockTransport responder. Returns false (silence) only when the device is asleep.
    bool respond(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
        if (asleep || request.size() < 8) {
            return false;
        }
        const uint8_t  unit  = request[0];
        const uint8_t  fn    = request[1];
        const uint16_t start = static_cast<uint16_t>((request[2] << 8) | request[3]);
        const uint16_t count = static_cast<uint16_t>((request[4] << 8) | request[5]);
        if (unit != unitId) {
            return false;  // not for us; a real bus stays quiet
        }
        ++reads;

        if (fn == modbus::kWriteSingleRegister) {
            // `count` holds the VALUE for 0x06, not a register count -- same two bytes, a
            // different meaning, which is exactly the confusion the echo is there to catch.
            const uint16_t value = count;
            if (registers.find(start) == registers.end() || refuseWrites) {
                reply = {unit, static_cast<uint8_t>(fn | 0x80),
                         static_cast<uint8_t>(refuseWrites ? 0x04 : 0x02)};
                appendCrc(reply);
                return true;
            }
            registers[start] = value;
            ++writes;
            lastWriteAddress = start;
            lastWriteValue   = value;
            // A real device echoes the request back verbatim. `echoWrongValue` makes it lie,
            // which is the only way to test that the caller checks the echo rather than
            // treating a well-formed reply as confirmation.
            reply = {unit, fn, request[2], request[3]};
            const uint16_t echoed = echoWrongValue ? static_cast<uint16_t>(value ^ 0xFFFF) : value;
            reply.push_back(static_cast<uint8_t>(echoed >> 8));
            reply.push_back(static_cast<uint8_t>(echoed & 0xFF));
            appendCrc(reply);
            return true;
        }

        std::vector<uint16_t> values;
        values.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            const auto it = registers.find(static_cast<uint16_t>(start + i));
            if (it == registers.end()) {
                // Illegal data address, exactly what a slave answers for a register it does
                // not implement -- and what a chain without a terminator produces.
                reply = {unit, static_cast<uint8_t>(fn | 0x80), 0x02};
                appendCrc(reply);
                return true;
            }
            values.push_back(it->second);
        }

        reply = {unit, fn, static_cast<uint8_t>(values.size() * 2)};
        for (const uint16_t v : values) {
            reply.push_back(static_cast<uint8_t>(v >> 8));
            reply.push_back(static_cast<uint8_t>(v & 0xFF));
        }
        appendCrc(reply);
        return true;
    }

    bool     asleep = false;
    uint32_t reads  = 0;  ///< how many requests were answered, for round-trip assertions

    /// Writes that landed, and the last one, so a test can assert WHICH register was written
    /// rather than only that the command reported success. A control write to the right value
    /// at the wrong address is the failure worth catching.
    uint32_t writes           = 0;
    uint16_t lastWriteAddress = 0;
    uint16_t lastWriteValue   = 0;
    /// Answer every write with exception 4 (device failure), as an inverter does for a register
    /// it will not accept -- locked, out of range, or wanting an installer code first.
    bool refuseWrites = false;
    /// Echo a value other than the one written. A device doing this is not doing what it was
    /// asked while looking, on the wire, like it did.
    bool echoWrongValue = false;

    /// Answer with a wrecked checksum, as a bus with a missing ground or no termination does.
    /// Distinct from `asleep`: the device is talking, the wire is mangling it -- and those two
    /// send an installer to look at completely different things.
    bool corruptCrc = false;

    /// Leave the first N replies intact before `corruptCrc` starts biting. Without this a test
    /// wrecks the very first read too, so the driver never gets past its opening exchange and
    /// everything downstream stays unexercised -- which is exactly how a swallowed status in the
    /// SunSpec chain walk survived its own test (review, 2026-07-25).
    uint32_t intactReplies = 0;

private:
    void appendCrc(std::vector<uint8_t>& frame) const {
        const uint16_t crc     = modbus::crc16(frame.data(), frame.size());
        const bool     corrupt = corruptCrc && reads > intactReplies;
        frame.push_back(static_cast<uint8_t>((crc & 0xFF) ^ (corrupt ? 0xFF : 0x00)));
        frame.push_back(static_cast<uint8_t>(crc >> 8));
    }

    static void writeString(std::vector<uint16_t>& block, size_t offset, const std::string& s,
                            size_t registerCount) {
        for (size_t i = 0; i < registerCount; ++i) {
            const size_t  hiIndex = i * 2;
            const uint8_t hi = hiIndex < s.size() ? static_cast<uint8_t>(s[hiIndex]) : 0;
            const uint8_t lo =
                hiIndex + 1 < s.size() ? static_cast<uint8_t>(s[hiIndex + 1]) : 0;
            block[offset + i] = static_cast<uint16_t>((hi << 8) | lo);
        }
    }
};

}  // namespace heliograph::test
