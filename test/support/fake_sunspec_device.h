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

private:
    static void appendCrc(std::vector<uint8_t>& frame) {
        const uint16_t crc = modbus::crc16(frame.data(), frame.size());
        frame.push_back(static_cast<uint8_t>(crc & 0xFF));
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
