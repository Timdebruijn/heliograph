#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decodes a raw EverSolar / Zeversolar PMU frame captured from the RS485 bus.

Paste a hex line straight from the firmware's TRACE log (set logging.level = trace):

    RS485 TX AA 55 01 00 00 00 10 00 00 01 10
    RS485 RX AA 55 00 10 01 00 11 82 1C ...

    python3 tools/decode_eversolar.py "AA 55 01 00 00 00 10 00 00 01 10"
    echo "RS485 RX AA 55 00 10 ..." | python3 tools/decode_eversolar.py

The "RS485 TX/RX" prefix, "0x" markers, colons and whitespace are all tolerated, so a line
copied verbatim from the serial monitor works as-is.

This is a Phase-3 aid: it makes a capture readable so the frame format, checksum and payload
layout can be checked against a real TL3000-20, and so the still-unknown fields (OP_MODE,
IMPEDANCE, NA_*) can be logged toward a future mapping. It mirrors the firmware exactly and
knows nothing the firmware does not: unknown fields are reported raw, never invented. The
authority is src/drivers/eversolar_legacy/ (eversolar_protocol.h, eversolar_parser.cpp) and
docs/eversolar-protocol.md; keep this file in step with them.
"""

from __future__ import annotations

import argparse
import sys

# --- Frame constants — keep in step with eversolar_protocol.h ----------------------------

HEADER = (0xAA, 0x55)
FRAME_OVERHEAD = (
    11  # 2 header + 2 src + 2 dst + 1 control + 1 function + 1 length + 2 checksum
)
OFF_SOURCE = 2
OFF_DESTINATION = 4
OFF_CONTROL = 6
OFF_FUNCTION = 7
OFF_LENGTH = 8
OFF_DATA = 9

PMU_ADDRESS = (0x01, 0x00)
BROADCAST_ADDRESS = (0x00, 0x00)
REGISTER_ACK = 0x06

# Command table from eversolar_protocol.h (namespace cmd). Keyed by (control, base function),
# where "base function" is the request code; responses set bit 7 (0x02 -> 0x82).
COMMANDS = {
    (0x10, 0x00): "OFFLINE_QUERY (register: serial number)",
    (0x10, 0x01): "SEND_REGISTER_ADDRESS (register: assign bus address)",
    (0x10, 0x04): "RE_REGISTER (register: broadcast, no response)",
    (0x11, 0x00): "QUERY_DESCRIPTION (read: model/description)",
    (0x11, 0x02): "QUERY_NORMAL_INFO (read: measurements)",
    (0x11, 0x03): "QUERY_INVERTER_ID (read: id string)",
}

# Single-string layout: 14 words / 28 bytes (eversolar_parser.cpp, namespace single).
NORMAL_INFO_SINGLE_BYTES = 28
# Dual-string layout: 16 words / 32 bytes (namespace dual).
NORMAL_INFO_DUAL_BYTES = 32
# Extended variants: the same leading words followed by an 8-word tail. A real TL3000-20 sends
# 44 (hardware capture 2026-07-19); the dual equivalent is 50. Kept in step with
# kNormalInfoSingleStringExtendedBytes / kNormalInfoDualStringExtendedBytes in the parser --
# this tool exists to mirror the firmware, and briefly did not: it reported a captured 44-byte
# frame as one the firmware would reject, while the device was decoding it fine (2026-07-20).
NORMAL_INFO_SINGLE_EXTENDED_BYTES = 44
NORMAL_INFO_DUAL_EXTENDED_BYTES = 50


# --- Parsing helpers ---------------------------------------------------------------------


def parse_hex(text: str) -> bytes:
    """Turns a pasted log line into bytes, tolerating the RS485 TX/RX prefix and 0x/`:`/space."""
    cleaned = text.strip()
    # Drop a leading "RS485 TX"/"RX" (or bare "TX"/"RX") prefix if present.
    for prefix in ("RS485 TX", "RS485 RX", "TX", "RX"):
        if cleaned.upper().startswith(prefix):
            cleaned = cleaned[len(prefix) :]
            break
    cleaned = (
        cleaned.replace("0x", " ")
        .replace("0X", " ")
        .replace(",", " ")
        .replace(":", " ")
    )
    tokens = cleaned.split()
    try:
        return bytes(int(t, 16) for t in tokens)
    except ValueError as exc:
        sys.exit(f"not valid hex: {exc}")


def word(data: bytes, index: int) -> int:
    """Big-endian uint16 at word index (mirrors readWord)."""
    o = index * 2
    return (data[o] << 8) | data[o + 1]


def signed_word(data: bytes, index: int) -> int:
    """Big-endian int16 at word index (mirrors readSignedWord). Used for TEMP."""
    v = word(data, index)
    return v - 0x10000 if v >= 0x8000 else v


def checksum(data: bytes) -> int:
    """Sum of all bytes, truncated to 16 bits (eversolar_protocol.h::checksum). Not a CRC."""
    return sum(data) & 0xFFFF


def addr_name(a: tuple[int, int]) -> str:
    if a == PMU_ADDRESS:
        return "PMU (this bridge)"
    if a == BROADCAST_ADDRESS:
        return "broadcast"
    if a[0] == 0x00:
        return f"inverter @ 0x{a[1]:02X}"
    return "unknown"


# --- Frame decode ------------------------------------------------------------------------


def decode_frame(data: bytes, layout: str) -> int:
    n = len(data)
    print(f"raw           {n} bytes: {data.hex(' ').upper()}")

    if n < FRAME_OVERHEAD:
        sys.exit(f"too short: need at least {FRAME_OVERHEAD} bytes, got {n}")

    if (data[0], data[1]) != HEADER:
        sys.exit(f"bad header: expected AA 55, got {data[0]:02X} {data[1]:02X}")

    source = (data[OFF_SOURCE], data[OFF_SOURCE + 1])
    destination = (data[OFF_DESTINATION], data[OFF_DESTINATION + 1])
    control = data[OFF_CONTROL]
    function = data[OFF_FUNCTION]
    length = data[OFF_LENGTH]

    expected_total = FRAME_OVERHEAD + length
    payload = data[OFF_DATA : OFF_DATA + length]

    is_response = bool(function & 0x80)
    base_function = function & 0x7F
    name = COMMANDS.get((control, base_function), "UNKNOWN command")

    print("header        AA 55  OK")
    print(f"source        {source[0]:02X} {source[1]:02X}  ({addr_name(source)})")
    print(
        f"destination   {destination[0]:02X} {destination[1]:02X}  ({addr_name(destination)})"
    )
    print(
        f"control/func  {control:02X} {function:02X}  "
        f"{'RESPONSE' if is_response else 'REQUEST'}: {name}"
    )
    print(f"data length   {length} (0x{length:02X})")

    # Length sanity against the actual byte count.
    if n != expected_total:
        print(
            f"length        MISMATCH: length byte implies {expected_total} bytes total, "
            f"buffer is {n} "
            f"({'frame split across reads — capture the whole line' if n < expected_total else 'trailing bytes'})"
        )
    else:
        print(f"length        OK: {expected_total} bytes total")

    # Checksum: sum over everything except the trailing 2 checksum bytes, big-endian.
    if n >= expected_total and length >= 0:
        body = data[: OFF_DATA + length]
        calc = checksum(body)
        stated = (data[OFF_DATA + length] << 8) | data[OFF_DATA + length + 1]
        ok = "OK" if calc == stated else "MISMATCH"
        print(f"checksum      stated {stated:#06x}, computed {calc:#06x}  {ok}")

    print(f"payload       {payload.hex(' ').upper() if payload else '(empty)'}")

    # Body-specific decode, only for responses whose format we know.
    if is_response and (control, base_function) == (0x11, 0x02):
        decode_normal_info(payload, layout)
    elif is_response and (control, base_function) == (0x10, 0x00):
        decode_ascii(payload, "serial number")
    elif is_response and (control, base_function) == (0x10, 0x01):
        ack = len(payload) >= 1 and payload[0] == REGISTER_ACK
        print(f"  ACK         {'yes (0x06)' if ack else 'NO — expected 0x06'}")
    elif is_response and (control, base_function) == (0x11, 0x03):
        decode_ascii(payload, "inverter id")

    return 0


def decode_ascii(payload: bytes, label: str) -> None:
    # Mirrors toTrimmedString: NUL terminates, keep printable, trim trailing spaces.
    chars = []
    for b in payload:
        if b == 0x00:
            break
        if 0x20 <= b <= 0x7E:
            chars.append(chr(b))
    text = "".join(chars).rstrip(" ")
    print(f"  {label:11} {text!r}")


def decode_normal_info(payload: bytes, layout: str) -> None:
    n = len(payload)

    if layout == "auto":
        if n in (NORMAL_INFO_SINGLE_BYTES, NORMAL_INFO_SINGLE_EXTENDED_BYTES):
            use_dual = False
        elif n in (NORMAL_INFO_DUAL_BYTES, NORMAL_INFO_DUAL_EXTENDED_BYTES):
            use_dual = True
        else:
            print(
                f"  layout      UnknownLayout: payload is {n} bytes, expected 28/44 "
                f"(single) or 32/50 (dual). Firmware would reject this frame (no data "
                f"published). Force with --layout single|dual if this device really uses "
                f"another length."
            )
            return
    elif layout == "single":
        if n < NORMAL_INFO_SINGLE_BYTES:
            print(
                f"  layout      LayoutMismatch: forced single needs >= 28 bytes, got {n}"
            )
            return
        use_dual = False
    else:  # dual
        if n < NORMAL_INFO_DUAL_BYTES:
            print(
                f"  layout      LayoutMismatch: forced dual needs >= 32 bytes, got {n}"
            )
            return
        use_dual = True

    print(
        f"  layout      {'dual-string (16 words)' if use_dual else 'single-string (14 words)'}"
        f"{'' if layout == 'auto' else ' [forced]'}"
    )

    if use_dual:
        _print_measurements(payload, dual=True)
    else:
        _print_measurements(payload, dual=False)


def _print_measurements(p: bytes, dual: bool) -> None:
    def row(field: str, mid: str, value: str) -> None:
        print(f"    {field:<10} {value:<16} {mid}")

    if not dual:
        # Word indices from eversolar_parser.cpp namespace single.
        temp = signed_word(p, 0) / 10.0
        e_today = word(p, 1) / 100.0
        vpv = word(p, 2) / 10.0
        ipv = word(p, 3) / 10.0
        iac = word(p, 4) / 10.0
        vac = word(p, 5) / 10.0
        freq = word(p, 6) / 100.0
        pac = word(p, 7)  # no scale factor: raw word is watts
        impedance = word(p, 8)
        e_total = ((word(p, 9) << 16) | word(p, 10)) / 10.0
        na_2 = word(p, 11)
        hours = word(p, 12)
        op_mode = word(p, 13)
        e_total_hi = word(p, 9)

        row("TEMP", "inverter.temperature", f"{temp:.1f} °C")
        row("E_TODAY", "energy.today", f"{e_today:.2f} kWh")
        row("VPV", "dc.mppt_1.voltage", f"{vpv:.1f} V")
        row("IPV", "dc.mppt_1.current", f"{ipv:.1f} A")
        row("IAC", "ac.phase_l1.current", f"{iac:.1f} A")
        row("VAC", "ac.phase_l1.voltage", f"{vac:.1f} V")
        row("FREQUENCY", "ac.frequency", f"{freq:.2f} Hz")
        row("PAC", "ac.power.total", f"{pac} W")
        row("E_TOTAL", "energy.total", f"{e_total:.1f} kWh")
        row("HOURS_UP", "inverter.operating_hours", f"{hours} h")
        _print_unknowns(op_mode, impedance=impedance, na={"NA_2 (word 11)": na_2})
        _print_e_total_note(e_total_hi)
    else:
        # Word indices from eversolar_parser.cpp namespace dual. IAC/VAC swap vs single, PAC moves.
        temp = signed_word(p, 0) / 10.0
        e_today = word(p, 1) / 100.0
        vpv = word(p, 2) / 10.0
        vpv2 = word(p, 3) / 10.0
        ipv = word(p, 4) / 10.0
        ipv2 = word(p, 5) / 10.0
        iac = word(p, 6) / 10.0
        vac = word(p, 7) / 10.0
        freq = word(p, 8) / 100.0
        pac = word(p, 9)
        na_0 = word(p, 10)
        e_total = ((word(p, 11) << 16) | word(p, 12)) / 10.0
        na_2 = word(p, 13)
        hours = word(p, 14)
        op_mode = word(p, 15)
        e_total_hi = word(p, 11)

        row("TEMP", "inverter.temperature", f"{temp:.1f} °C")
        row("E_TODAY", "energy.today", f"{e_today:.2f} kWh")
        row("VPV", "dc.mppt_1.voltage", f"{vpv:.1f} V")
        row("VPV2", "dc.mppt_2.voltage", f"{vpv2:.1f} V")
        row("IPV", "dc.mppt_1.current", f"{ipv:.1f} A")
        row("IPV2", "dc.mppt_2.current", f"{ipv2:.1f} A")
        row("IAC", "ac.phase_l1.current", f"{iac:.1f} A")
        row("VAC", "ac.phase_l1.voltage", f"{vac:.1f} V")
        row("FREQUENCY", "ac.frequency", f"{freq:.2f} Hz")
        row("PAC", "ac.power.total", f"{pac} W")
        row("E_TOTAL", "energy.total", f"{e_total:.1f} kWh")
        row("HOURS_UP", "inverter.operating_hours", f"{hours} h")
        _print_unknowns(
            op_mode, impedance=None, na={"NA_0 (word 10)": na_0, "NA_2 (word 13)": na_2}
        )
        _print_e_total_note(e_total_hi)


def _print_unknowns(op_mode: int, impedance: int | None, na: dict[str, int]) -> None:
    print(
        "  unknown fields (logged raw, never mapped to text — see docs/eversolar-protocol.md):"
    )
    print(
        f"    OP_MODE    status_code = {op_mode} (0x{op_mode:04X})  -> status_text "
        f'"Unknown ({op_mode})"'
    )
    if impedance is not None:
        print(
            f"    IMPEDANCE  raw = {impedance} (0x{impedance:04X})  unit/scale unknown, "
            f"not published"
        )
    for label, value in na.items():
        print(f"    {label:<10} raw = {value} (0x{value:04X})  unknown, not published")


def _print_e_total_note(e_total_hi: int) -> None:
    if e_total_hi:
        delta = e_total_hi * 0.1
        print(
            f"  note        E_TOTAL_HI = {e_total_hi}; our uint32 value is +{delta:.1f} kWh "
            f"vs eversolar-monitor (its /65535 bug). Expected, not an error."
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "frame", nargs="?", help="hex bytes of one frame (default: read from stdin)"
    )
    parser.add_argument(
        "--layout",
        choices=("auto", "single", "dual"),
        default="auto",
        help="payload layout for QUERY_NORMAL_INFO; auto derives it from length "
        "(default), single/dual force it",
    )
    args = parser.parse_args()

    text = args.frame if args.frame is not None else sys.stdin.read()
    if not text.strip():
        parser.error("no frame given (pass hex as an argument or on stdin)")

    return decode_frame(parse_hex(text), args.layout)


if __name__ == "__main__":
    raise SystemExit(main())
