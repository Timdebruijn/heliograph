// SPDX-License-Identifier: MIT
//
// A simulated EverSolar inverter, driven by what is asked of it rather than by a fixed reply
// order. Tests describe the device ("it is offline", "it sends a dual-string payload") instead
// of the driver's internal write sequence, so they keep testing behaviour rather than
// implementation detail.
//
// This is where EverSolar framing knowledge lives on the test side; MockTransport stays
// protocol-agnostic.

#pragma once

#include <vector>

#include "drivers/eversolar_legacy/eversolar_parser.h"  // pulls in the pmu framing + eversolar names
#include "fixtures/eversolar_frames.h"
#include "support/mock_transport.h"

namespace heliograph::test {

class FakeEversolarDevice {
public:
    enum class Payload { SingleString, DualString, Night, BadChecksum, BadLength };

    /// Off the bus entirely, as every solar inverter is after sunset.
    bool offline = false;
    /// Refuses the address assignment.
    bool refuseRegistration = false;
    /// What QUERY_NORMAL_INFO returns.
    Payload payload = Payload::SingleString;
    /// Echo our own request back before answering, as a half-duplex bus can.
    bool echoRequests = false;
    /// Prepend line noise to each reply.
    bool prependNoise = false;

    uint32_t reRegisterCount = 0;
    uint32_t normalInfoCount = 0;

    /// Requires registration before answering QUERY_NORMAL_INFO, like the real protocol.
    bool registered = false;

    /// Answer the offline query with the frame captured from a real TL3000-20 on 2026-07-19
    /// (source 00 00, NUL-terminated 16-char serial) instead of the constructed fixture.
    bool useCapturedOfflineQuery = false;

    /// Stay silent on this many QUERY_NORMAL_INFO requests, then answer again. One lost reply
    /// on a real bus must not cost the driver its registration.
    uint32_t silentNormalInfoPolls = 0;

    /// Registered and answering registration/id queries, but withholding QUERY_NORMAL_INFO
    /// indefinitely -- the dawn state observed on 2026-07-21, where a TL3000-20 near its start
    /// threshold gave its id but no measurement data. Distinct from `offline` (which forgets
    /// the address): here the address is kept, so an offline query stays ignored.
    bool withholdNormalInfo = false;

    Responder responder() {
        return [this](const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
            return handle(request, reply);
        };
    }

    void installOn(MockTransport& transport) { transport.setResponder(responder()); }

private:
    static void append(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
        out.insert(out.end(), p, p + n);
    }

    bool handle(const std::vector<uint8_t>& request, std::vector<uint8_t>& reply) {
        using namespace heliograph::eversolar;
        if (request.size() < kFrameOverhead) {
            return false;
        }
        const uint8_t control  = request[kOffsetControl];
        const uint8_t function = request[kOffsetFunction];

        // RE_REGISTER is a broadcast with no defined response. A real device answers nothing;
        // it just forgets its address.
        if (control == 0x10 && function == 0x04) {
            ++reRegisterCount;
            registered = false;
            return false;
        }

        if (offline) {
            // Powerless overnight: the volatile bus address is forgotten, so the device comes
            // back unregistered at sunrise -- which is why plain re-registration recovers it.
            registered = false;
            return false;
        }

        std::vector<uint8_t> body;
        if (control == 0x10 && function == 0x00) {
            if (registered) {
                // Like the real device (observed 2026-07-19): only an UNregistered inverter
                // answers the offline query. This asymmetry is why losing the driver-side
                // registration state while the inverter keeps its address needs RE_REGISTER
                // to recover -- an offline query alone stays unanswered forever.
                return false;
            }
            if (useCapturedOfflineQuery) {
                append(body, fixtures::kRespOfflineQueryCaptured,
                       fixtures::kRespOfflineQueryCapturedLen);
            } else {
                append(body, fixtures::kRespOfflineQuery, fixtures::kRespOfflineQueryLen);
            }
        } else if (control == 0x10 && function == 0x01) {
            if (refuseRegistration) {
                append(body, fixtures::kRespRegisterNak, fixtures::kRespRegisterNakLen);
            } else {
                registered = true;
                append(body, fixtures::kRespRegisterAck, fixtures::kRespRegisterAckLen);
            }
        } else if (control == 0x11 && function == 0x03) {
            append(body, fixtures::kRespInverterId, fixtures::kRespInverterIdLen);
        } else if (control == 0x11 && function == 0x02) {
            if (!registered) {
                return false;  // unknown address: silence, exactly as the real device does
            }
            if (withholdNormalInfo) {
                return false;  // registered, id-answering, but not producing measurements yet
            }
            if (silentNormalInfoPolls > 0) {
                --silentNormalInfoPolls;
                return false;  // dropped reply; the device itself stays registered
            }
            ++normalInfoCount;
            switch (payload) {
                case Payload::SingleString:
                    append(body, fixtures::kRespNormalInfoSingle, fixtures::kRespNormalInfoSingleLen);
                    break;
                case Payload::DualString:
                    append(body, fixtures::kRespNormalInfoDual, fixtures::kRespNormalInfoDualLen);
                    break;
                case Payload::Night:
                    append(body, fixtures::kRespNormalInfoNight, fixtures::kRespNormalInfoNightLen);
                    break;
                case Payload::BadChecksum:
                    append(body, fixtures::kRespBadChecksum, fixtures::kRespBadChecksumLen);
                    break;
                case Payload::BadLength:
                    append(body, fixtures::kRespBadPayloadLength, fixtures::kRespBadPayloadLengthLen);
                    break;
            }
        } else {
            return false;  // unknown request: a real device stays quiet
        }

        if (prependNoise) {
            const uint8_t noise[] = {0x00, 0xFF, 0xAA, 0x13, 0x37};
            append(reply, noise, sizeof(noise));
        }
        if (echoRequests) {
            reply.insert(reply.end(), request.begin(), request.end());
        }
        reply.insert(reply.end(), body.begin(), body.end());
        return true;
    }
};

}  // namespace heliograph::test
