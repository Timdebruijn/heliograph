// SPDX-License-Identifier: MIT
//
// A simulated SolaX X1 inverter, driven by what is asked of it — same philosophy as
// FakeEversolarDevice. Builds its frames on the fly through the shared pmu framing, so the
// test-side protocol knowledge is the payload layouts only.

#pragma once

#include <cstring>
#include <vector>

#include "drivers/solax_x1/solax_parser.h"
#include "support/mock_transport.h"

namespace heliograph::test {

class FakeSolaxDevice {
public:
    /// Off the bus entirely (night). Forgets the volatile address, like the real device.
    bool offline = false;
    /// Withhold the 0x06 ACK on address assignment. The reference implementation never
    /// waits for the ACK, which suggests real firmware may not always send one; the driver
    /// must then verify by status query instead.
    bool withholdAck = false;
    /// Registered from a previous master session: ignores offline queries but answers
    /// addressed queries. The cold-boot-against-registered-inverter scenario. When a test
    /// sets this directly it must also set assignedAddress to the address the device holds.
    bool registered = false;
    /// Prepend line noise to each reply.
    bool prependNoise = false;

    /// Values served in the status report (G1-style 52-byte payload).
    int16_t  temperatureC   = 34;
    uint16_t energyToday10  = 71;      ///< 7.1 kWh
    uint16_t pv1Voltage10   = 2103;    ///< 210.3 V
    uint16_t pv2Voltage10   = 0;
    uint16_t pv1Current10   = 41;      ///< 4.1 A
    uint16_t pv2Current10   = 0;
    uint16_t acCurrent10    = 37;      ///< 3.7 A
    uint16_t acVoltage10    = 2318;    ///< 231.8 V
    uint16_t frequency100   = 4999;    ///< 49.99 Hz
    uint16_t acPowerW       = 856;
    uint32_t energyTotal10  = 123456;  ///< 12345.6 kWh
    uint32_t runtimeHours   = 9876;
    uint16_t mode           = 2;       ///< Normal
    uint32_t errorBits      = 0;

    uint8_t assignedAddress = 0;
    uint32_t statusQueryCount = 0;
    uint32_t offlineQueryCount = 0;

    static constexpr uint8_t kSerial[solax::kSerialNumberBytes] = {
        'X', '1', 'M', 'I', 'N', 'I', '1', '1', '0', '0', 'A', 'B', 'C', 'D'};

    Responder responder() {
        return [this](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
            return handle(request, reply);
        };
    }

    void installOn(MockTransport& transport) { transport.setResponder(responder()); }

private:
    static void putU16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    static void putU32Big(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    static void putU32Little(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v >> 16));
        out.push_back(static_cast<uint8_t>(v >> 24));
    }
    static void putAscii(std::vector<uint8_t>& out, const char* s, size_t width) {
        size_t i = 0;
        for (; s[i] != '\0' && i < width; ++i) {
            out.push_back(static_cast<uint8_t>(s[i]));
        }
        for (; i < width; ++i) {
            out.push_back(' ');
        }
    }

    /// One complete response frame through the real pmu builder, so checksum and layout can
    /// never drift from the implementation under test.
    void frame(std::vector<uint8_t>& reply, heliograph::pmu::Address source, uint8_t control,
               uint8_t function, const std::vector<uint8_t>& body) {
        using namespace heliograph::pmu;
        uint8_t buf[kMaxFrameSize];
        size_t  len = 0;
        // Responses go PMU-ward: destination 01 00.
        CommandCode responseCode{control, static_cast<uint8_t>(function & 0x7F)};
        // buildRequestFrom writes the *request* function; patch to the response function.
        if (buildRequestFrom(source, responseCode, kPmuAddress, body.data(), body.size(), buf,
                             sizeof(buf), len) != BuildResult::Ok) {
            return;
        }
        buf[kOffsetFunction] = function;
        // Re-checksum after the patch.
        const uint16_t sum = checksum(buf, kOffsetData + body.size());
        buf[kOffsetData + body.size()]     = static_cast<uint8_t>(sum >> 8);
        buf[kOffsetData + body.size() + 1] = static_cast<uint8_t>(sum & 0xFF);
        if (prependNoise) {
            const uint8_t noise[] = {0x00, 0xFF, 0x13};
            reply.insert(reply.end(), noise, noise + sizeof(noise));
        }
        reply.insert(reply.end(), buf, buf + len);
    }

    std::vector<uint8_t> statusBody() const {
        std::vector<uint8_t> b;
        putU16(b, static_cast<uint16_t>(temperatureC));
        putU16(b, energyToday10);
        putU16(b, pv1Voltage10);
        putU16(b, pv2Voltage10);
        putU16(b, pv1Current10);
        putU16(b, pv2Current10);
        putU16(b, acCurrent10);
        putU16(b, acVoltage10);
        putU16(b, frequency100);
        putU16(b, acPowerW);
        putU16(b, 0);  // offset 20 unused
        putU32Big(b, energyTotal10);
        putU32Big(b, runtimeHours);
        putU16(b, mode);
        for (int i = 0; i < 7; ++i) {
            putU16(b, 0);  // fault threshold words 32..44
        }
        putU32Little(b, errorBits);
        putU16(b, 0);  // G1 tail: CT Pgrid
        return b;      // 52 bytes
    }

    std::vector<uint8_t> deviceInfoBody() const {
        std::vector<uint8_t> b;
        b.push_back(0x01);  // device type
        putAscii(b, "1100", 6);
        putAscii(b, "1.09", 5);
        putAscii(b, "X1-1.1-S-D", 14);
        putAscii(b, "SolaxPower", 14);
        putAscii(b, "XM3B11ABCDEF", 14);
        putAscii(b, "380", 4);
        return b;  // 58 bytes
    }

    bool handle(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
        using namespace heliograph::pmu;
        if (request.size() < kFrameOverhead) {
            return false;
        }
        const uint8_t control  = request[kOffsetControl];
        const uint8_t function = request[kOffsetFunction];
        const uint8_t destLow  = request[kOffsetDestinationHigh + 1];
        const uint8_t dataLen  = request[kOffsetLength];

        if (offline) {
            registered = false;  // volatile address lost with power
            return false;
        }

        if (control == 0x10 && function == 0x00) {
            ++offlineQueryCount;
            if (registered) {
                return false;  // family asymmetry: a registered inverter ignores this
            }
            std::vector<uint8_t> body(kSerial, kSerial + sizeof(kSerial));
            frame(reply, kBroadcastAddress, 0x10, 0x80, body);
            return true;
        }

        if (control == 0x10 && function == 0x01) {
            // The reference sends SEND_ADDRESS with source 00 00, not the PMU address --
            // the very reason buildRequestFrom() exists. Enforcing it here means a
            // regression that silently reverts to the PMU source fails these tests.
            if (request[kOffsetSourceHigh] != 0x00 || request[kOffsetSourceHigh + 1] != 0x00) {
                return false;
            }
            // Serial must be echoed byte-exact, followed by the address.
            if (dataLen != solax::kSerialNumberBytes + 1 ||
                std::memcmp(request.data() + kOffsetData, kSerial, sizeof(kSerial)) != 0) {
                return false;
            }
            registered      = true;
            assignedAddress = request[kOffsetData + solax::kSerialNumberBytes];
            if (withholdAck) {
                return false;  // took the address, said nothing -- like the reference expects
            }
            std::vector<uint8_t> body{kRegisterAck};
            frame(reply, inverterAddress(assignedAddress), 0x10, 0x81, body);
            return true;
        }

        if (!registered || destLow != assignedAddress) {
            return false;  // unknown address: silence, like the real device
        }

        if (control == 0x11 && function == 0x02) {
            ++statusQueryCount;
            frame(reply, inverterAddress(assignedAddress), 0x11, 0x82, statusBody());
            return true;
        }
        if (control == 0x11 && function == 0x03) {
            frame(reply, inverterAddress(assignedAddress), 0x11, 0x83, deviceInfoBody());
            return true;
        }
        return false;
    }
};

}  // namespace heliograph::test
