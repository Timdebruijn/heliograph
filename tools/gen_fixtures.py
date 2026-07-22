#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates test/fixtures/eversolar_frames.h.

These fixtures are CONSTRUCTED from the protocol as reverse-engineered from
eversolar-monitor (see docs/eversolar-protocol.md). They prove the parser implements our
interpretation of the protocol; they do NOT prove that interpretation is correct. Only
Phase 3, against the real TL3000-20, can do that. Recorded frames land here afterwards.

Run: python3 tools/gen_fixtures.py
"""

from __future__ import annotations

HEADER = (0xAA, 0x55)
PMU = (0x01, 0x00)
INVERTER_ADDR = 0x10


def checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


def frame(src: tuple[int, int], dst: tuple[int, int], ctrl: int, func: int,
          payload: bytes = b"") -> bytes:
    body = bytes([*HEADER, *src, *dst, ctrl, func, len(payload)]) + payload
    return body + checksum(body).to_bytes(2, "big")


def request(ctrl: int, func: int, dst_low: int, payload: bytes = b"") -> bytes:
    return frame(PMU, (0x00, dst_low), ctrl, func, payload)


def response(ctrl: int, func: int, payload: bytes, src_low: int = INVERTER_ADDR) -> bytes:
    return frame((0x00, src_low), PMU, ctrl, func | 0x80, payload)


def words(*values: int) -> bytes:
    return b"".join(v.to_bytes(2, "big") for v in values)


# --- Realistic single-string payload (14 words / 28 bytes) ------------------------------
# Values chosen to match the worked example in the docs, including energy.total = 18452.7
# kWh, which needs the high word (2) and therefore exercises the uint32 path where the
# reference implementation's *65535 bug shows up.
E_TOTAL_DECI = 184527          # 18452.7 kWh in units of 0.1 kWh
E_TOTAL_HI = E_TOTAL_DECI >> 16      # 2
E_TOTAL_LO = E_TOTAL_DECI & 0xFFFF   # 53455

SINGLE_STRING = words(
    413,          # 0  TEMP        41.3 C
    842,          # 1  E_TODAY     8.42 kWh
    3412,         # 2  VPV         341.2 V
    56,           # 3  IPV         5.6 A
    79,           # 4  IAC         7.9 A
    2334,         # 5  VAC         233.4 V
    4998,         # 6  FREQUENCY   49.98 Hz
    1842,         # 7  PAC         1842 W
    0,            # 8  IMPEDANCE   unknown unit
    E_TOTAL_HI,   # 9  E_TOTAL_HI
    E_TOTAL_LO,   # 10 E_TOTAL_LO  -> 18452.7 kWh
    0,            # 11 NA_2        unknown
    31204,        # 12 HOURS_UP
    1,            # 13 OP_MODE     meaning undocumented
)

# --- Dual-string payload (16 words / 32 bytes) -----------------------------------------
# Note the different field order: IAC/VAC swap and PAC moves. Same physical values as the
# single-string fixture so a layout mix-up in the parser shows up as wrong numbers.
DUAL_STRING = words(
    413,          # 0  TEMP
    842,          # 1  E_TODAY
    3412,         # 2  VPV
    3350,         # 3  VPV2        335.0 V
    56,           # 4  IPV
    48,           # 5  IPV2        4.8 A
    79,           # 6  IAC
    2334,         # 7  VAC
    4998,         # 8  FREQUENCY
    1842,         # 9  PAC
    0,            # 10 NA_0
    E_TOTAL_HI,   # 11 E_TOTAL_HI
    E_TOTAL_LO,   # 12 E_TOTAL_LO
    0,            # 13 NA_2
    31204,        # 14 HOURS_UP
    1,            # 15 OP_MODE
)

# Negative temperature: -0.1 C is 0xFFFF per the reference's comment.
NIGHT_SINGLE = words(
    0xFFFF,       # 0  TEMP        -0.1 C
    0,            # 1  E_TODAY     0.00 kWh (a real zero: nothing produced yet)
    0,            # 2  VPV
    0,            # 3  IPV
    0,            # 4  IAC
    0,            # 5  VAC
    0,            # 6  FREQUENCY
    0,            # 7  PAC
    0,            # 8  IMPEDANCE
    E_TOTAL_HI,   # 9  E_TOTAL_HI
    E_TOTAL_LO,   # 10 E_TOTAL_LO
    0,            # 11 NA_2
    31204,        # 12 HOURS_UP
    0,            # 13 OP_MODE
)

SERIAL = b"XH300060115506193600V610"
# The id string a real TL3000-20 sent on 2026-07-19 (QUERY_INVERTER_ID during Phase 3), byte
# for byte including the space padding. The earlier constructed guess had the fields in a
# different order; hardware disagreed. Field meaning (single sample, see eversolar-protocol.md):
# phases?, rating+firmware, model, manufacturer, extended serial.
INVERTER_ID = b"1  3000E1.00   TL3000-20    Ever-Solar      XH300060115506193600V610-01023-06"
EXPECTED_MODEL = "TL3000-20"

FIXTURES: list[tuple[str, str, bytes]] = [
    ("kReqQueryNormalInfo", "Request: QUERY_NORMAL_INFO to inverter at 0x10",
     request(0x11, 0x02, INVERTER_ADDR)),
    ("kReqOfflineQuery", "Request: OFFLINE_QUERY, broadcast",
     request(0x10, 0x00, 0x00)),
    ("kReqReRegister", "Request: RE_REGISTER, broadcast, no response expected",
     request(0x10, 0x04, 0x00)),
    ("kReqSendAddress", "Request: SEND_REGISTER_ADDRESS = serial + assigned address",
     request(0x10, 0x01, 0x00, SERIAL + bytes([INVERTER_ADDR]))),
    ("kRespNormalInfoSingle", "Response: QUERY_NORMAL_INFO, single-string layout (28 bytes)",
     response(0x11, 0x02, SINGLE_STRING)),
    ("kRespNormalInfoDual", "Response: QUERY_NORMAL_INFO, dual-string layout (32 bytes)",
     response(0x11, 0x02, DUAL_STRING)),
    ("kRespNormalInfoNight", "Response: QUERY_NORMAL_INFO at night, negative temp, real zeros",
     response(0x11, 0x02, NIGHT_SINGLE)),
    # Source 00 00, not 00 10: an unregistered inverter has no address yet -- answering from
    # the address it is *about to be assigned* was a constructed-fixture guess, disproven by
    # hardware on 2026-07-19 (see kRespOfflineQueryCaptured). The reference never checks
    # sources at all, so it could not have told us.
    ("kRespOfflineQuery", "Response: OFFLINE_QUERY carrying the serial number (source 00 00)",
     response(0x10, 0x00, SERIAL, src_low=0x00)),
    # Byte-for-byte the frame a real TL3000-20 answered with on 2026-07-19 (Phase 3, first
    # contact). NUL-terminated 16-char serial plus one trailing byte of meaning unknown.
    ("kRespOfflineQueryCaptured", "Response: OFFLINE_QUERY captured from a real TL3000-20",
     response(0x10, 0x00, b"XH30006011550619\x00\x01", src_low=0x00)),
    # The first QUERY_NORMAL_INFO a real TL3000-20 ever answered us (2026-07-19 12:23:57,
    # mid-production). 44-byte payload: the first 14 words are exactly the single-string
    # layout (38.6 degC, 3.44 kWh today, 346.4 V / 2.0 A PV, 230.6 V / 2.7 A AC, 50.03 Hz,
    # 655 W, 35445.9 kWh lifetime, 47401 h, OP_MODE 1), followed by an 8-word tail of zeros
    # and FFFF. Neither 28 nor 32: the length-decides-layout hypothesis is hereby disproven,
    # and the reference making `strings` a config option finally makes sense -- it only ever
    # indexes words and ignores everything after.
    ("kRespNormalInfoCaptured", "Response: QUERY_NORMAL_INFO captured from a real TL3000-20"
     " (44-byte payload: single-string words plus unknown tail)",
     response(0x11, 0x02, words(
         0x0182, 0x0158, 0x0D88, 0x0014, 0x001B, 0x0902, 0x138B, 0x028F, 0xFFFF, 0x0005,
         0x689B, 0x0000, 0xB929, 0x0001, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000,
         0xFF00, 0x0000))),
    # DERIVED, not captured: the dual-string words followed by a 9-word tail, 50 bytes total,
    # matching the payload length and word offsets of the calibrated Zeversolar 2000s capture
    # in the ha-zeversolar-modbus project (2026-05-30). We have no raw bytes from that unit;
    # this exercises the "dual generation with tail" length until someone captures one.
    ("kRespNormalInfoDualExtended", "Response: QUERY_NORMAL_INFO, dual-generation 50-byte"
     " payload (dual words + unknown tail)",
     response(0x11, 0x02, DUAL_STRING + words(
         0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000))),
    ("kRespRegisterAck", "Response: SEND_REGISTER_ADDRESS acknowledged (0x06)",
     response(0x10, 0x01, bytes([0x06]))),
    ("kRespRegisterNak", "Response: SEND_REGISTER_ADDRESS not acknowledged",
     response(0x10, 0x01, bytes([0x15]))),
    ("kRespInverterId", "Response: QUERY_INVERTER_ID ASCII string",
     response(0x11, 0x03, INVERTER_ID)),
]


def corrupt_checksum(f: bytes) -> bytes:
    b = bytearray(f)
    b[-1] ^= 0xFF
    return bytes(b)


def wrong_source(f: bytes) -> bytes:
    b = bytearray(f)
    b[3] = 0x99  # source low byte -> some other inverter
    body = bytes(b[:-2])
    return body + checksum(body).to_bytes(2, "big")


def wrong_destination(f: bytes) -> bytes:
    b = bytearray(f)
    b[5] = 0x42  # destination low byte -> not the PMU
    body = bytes(b[:-2])
    return body + checksum(body).to_bytes(2, "big")


def bad_payload_length(f: bytes) -> bytes:
    # 30-byte payload: neither layout. Truncate a dual-string frame's payload.
    payload = DUAL_STRING[:30]
    return response(0x11, 0x02, payload)


_normal = response(0x11, 0x02, SINGLE_STRING)

FIXTURES += [
    ("kRespBadChecksum", "Corrupt: last checksum byte flipped",
     corrupt_checksum(_normal)),
    ("kRespWrongSource", "Corrupt: correct checksum, but from a different inverter address",
     wrong_source(_normal)),
    ("kRespWrongDestination", "Corrupt: correct checksum, but not addressed to the PMU",
     wrong_destination(_normal)),
    ("kRespBadPayloadLength", "Corrupt: 30-byte payload, matches neither layout",
     bad_payload_length(_normal)),
    ("kRespBadHeader", "Corrupt: header is not AA 55",
     bytes([0xAB, 0x55]) + _normal[2:]),
    ("kAllZeroFrame", "Corrupt: all zeros. Checksum 'passes' (0 == 0) but is not data.",
     bytes(11)),
    ("kRespPartial", "Partial: first 12 bytes of a valid response",
     _normal[:12]),
]


def format_bytes(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    return "\n".join(lines)


def main() -> None:
    # The generator must reproduce the hardware capture of 2026-07-19 byte for byte. If a
    # framing detail here ever drifts from what the real inverter sends, this trips before a
    # wrong fixture reaches the tests.
    captured = bytes.fromhex(
        "aa 55 00 00 01 00 10 80 12 58 48 33 30 30 30 36 30 31 31 35 35 30 36 31 39 00 01"
        " 05 08".replace(" ", ""))
    generated = dict((n, f) for n, _, f in FIXTURES)["kRespOfflineQueryCaptured"]
    assert generated == captured, "kRespOfflineQueryCaptured drifted from the hardware capture"

    captured_info = bytes.fromhex(
        "aa 55 00 10 01 00 11 82 2c 01 82 01 58 0d 88 00 14 00 1b 09 02 13 8b 02 8f ff ff"
        " 00 05 68 9b 00 00 b9 29 00 01 00 00 00 00 ff ff 00 00 00 00 00 00 ff 00 00 00"
        " 0b 8f".replace(" ", ""))
    generated_info = dict((n, f) for n, _, f in FIXTURES)["kRespNormalInfoCaptured"]
    assert generated_info == captured_info, \
        "kRespNormalInfoCaptured drifted from the hardware capture"

    out = [
        "// SPDX-License-Identifier: MIT",
        "//",
        "// GENERATED by tools/gen_fixtures.py -- do not edit by hand.",
        "//",
        "// These frames are CONSTRUCTED from the protocol as reverse-engineered from",
        "// eversolar-monitor (MIT, (c) 2021 Henrik Moller Jorgensen). They verify that the",
        "// parser implements our interpretation of the protocol -- not that the",
        "// interpretation is correct. See test/fixtures/README.md.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace heliograph::fixtures {",
        "",
    ]
    for name, comment, data in FIXTURES:
        out.append(f"// {comment}")
        out.append(f"inline constexpr uint8_t {name}[] = {{")
        out.append(format_bytes(data))
        out.append("};")
        out.append(f"inline constexpr size_t {name}Len = sizeof({name});")
        out.append("")

    out.append("// Expected physical values for kRespNormalInfoSingle / kRespNormalInfoDual.")
    out.append("namespace expected {")
    out.append("inline constexpr double kTemperatureC   = 41.3;")
    out.append("inline constexpr double kEnergyTodayKwh = 8.42;")
    out.append("inline constexpr double kEnergyTotalKwh = 18452.7;")
    out.append("inline constexpr double kAcVoltage      = 233.4;")
    out.append("inline constexpr double kAcCurrent      = 7.9;")
    out.append("inline constexpr double kAcPowerW       = 1842.0;")
    out.append("inline constexpr double kFrequencyHz    = 49.98;")
    out.append("inline constexpr double kPvVoltage1     = 341.2;")
    out.append("inline constexpr double kPvCurrent1     = 5.6;")
    out.append("inline constexpr double kPvVoltage2     = 335.0;")
    out.append("inline constexpr double kPvCurrent2     = 4.8;")
    out.append("inline constexpr uint32_t kOperatingHours = 31204;")
    out.append("inline constexpr uint16_t kStatusCode     = 1;")
    out.append("")
    out.append("// What eversolar-monitor reports for the same frame, because of its")
    out.append("// `high * 65535` instead of `high * 65536`. Our value is high*0.1 kWh higher.")
    out.append(f"inline constexpr double kEnergyTotalKwhReferenceBug = "
               f"{(E_TOTAL_LO / 10) + (E_TOTAL_HI * 65535 / 10):.1f};")
    out.append(f"inline constexpr double kEnergyTotalExpectedDelta = {E_TOTAL_HI * 0.1:.1f};")
    out.append("}  // namespace expected")
    out.append("")
    out.append("inline constexpr const char* kExpectedSerial = \""
               + SERIAL.decode() + "\";")
    out.append("inline constexpr const char* kExpectedInverterId = \""
               + INVERTER_ID.decode().rstrip() + "\";")
    out.append("inline constexpr const char* kExpectedModel = \"" + EXPECTED_MODEL + "\";")
    out.append("")
    out.append("}  // namespace heliograph::fixtures")
    out.append("")

    path = "test/fixtures/eversolar_frames.h"
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"wrote {path} ({len(FIXTURES)} fixtures)")


if __name__ == "__main__":
    main()
