#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerates the dashboard screenshots in docs/images/.

The page ships as a C++ string literal and fetches its data over REST, so a screenshot needs
three things standing at once: the HTML out of the header, a server answering the API calls,
and a browser that can click a tab. This does all three.

It was done by hand for PR #56 and the recipe lived nowhere, so the next round meant working it
out again from the results. Committed for that reason rather than because it runs often.

    # capture fresh data from a running bridge, sanitise it, and shoot every tab
    python3 tools/make_screenshots.py --bridge 192.168.20.254

    # re-shoot from the captured fixture, no bridge needed
    python3 tools/make_screenshots.py

DATA IS SANITISED ON CAPTURE, not on render: serial numbers, the MAC-derived hostname, the WiFi
SSID and the NTP server are replaced before anything is written to disk. The fixture is
committed and a screenshot is published, so a real serial reaching either is a real leak, and
the only safe place to stop it is the moment the payload arrives.
"""

from __future__ import annotations

import argparse
import http.server
import json
import pathlib
import re
import shutil
import socketserver
import subprocess
import sys
import threading
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSET = ROOT / "src/web/assets/index_html.h"
FIXTURE = ROOT / "docs/images/fixture.json"

# Endpoint -> filename stem. The page GETs exactly these; anything else it asks for gets a 404,
# which is what a bridge with the feature switched off would answer anyway.
ENDPOINTS = ["status", "config", "diagnostics", "drivers", "discovery", "capture"]

# data-t value -> output file. Tabs are switched by CLICK, not by URL, so each shot navigates to
# the same page and clicks its way there.
TABS = {
    "dash": "dashboard.png",
    "dev": "device.png",
    "diag": "diagnostics.png",
    "cfg": "settings.png",
    "logs": "logs.png",
}


def sanitise(payload):
    """Replaces anything identifying with a plausible stand-in, in place.

    Plausible rather than blanked: a screenshot showing `----` teaches a reader nothing about
    what the field looks like in use, which is the entire point of the screenshot.
    """
    if isinstance(payload, dict):
        return {k: _field(k, v) for k, v in payload.items()}
    if isinstance(payload, list):
        return [sanitise(v) for v in payload]
    return payload


def _field(key: str, value):
    if isinstance(value, (dict, list)):
        return sanitise(value)
    if not isinstance(value, str):
        return value
    if key in ("serial_number", "serial"):
        return "SN0000000000"
    if key == "hostname":
        return "heliograph-a1b2c3"
    if key == "ssid":
        return "HomeNetwork"
    if key in ("ntp_server",):
        return "pool.ntp.org"
    if key in ("id", "instance_key", "device"):
        # Two different shapes wear the same key. The BRIDGE id is heliograph-<mac tail>, in
        # lowercase hex; a DEVICE id is driver-<serial>, usually uppercase. The first version
        # only substituted uppercase runs and let heliograph-56a11c straight through into a
        # committed fixture.
        #
        # The bridge id gets the same stand-in as the hostname, because on a real device both
        # come from the same MAC and a reader who sees them disagree learns something false.
        if value.startswith("heliograph-"):
            return "heliograph-a1b2c3"
        # Shape kept -- driver-SERIAL -- because the id is what MQTT topics and the Modbus unit
        # mapping are keyed on, and a reader needs to recognise the form.
        return re.sub(r"[A-Za-z0-9]{8,}$", "0000000001", value)
    if re.fullmatch(r"(\d{1,3}\.){3}\d{1,3}", value):
        return "192.168.1.50"
    return value


# Shapes that must never survive sanitisation. Checked after the fact rather than trusted,
# because the fixture is committed and the screenshots are published: a rule that silently stops
# matching is a leak nobody notices, and the first version of the id rule did exactly that.
LEAK_PATTERNS = {
    "MAC-derived name": re.compile(r"heliograph-[0-9a-f]{6}"),
    "serial-shaped run": re.compile(r"\b[A-Z]{2,}[0-9]{6,}\b"),
    "private IPv4": re.compile(
        r"\b(?:192\.168\.\d{1,3}|10\.\d{1,3}\.\d{1,3}"
        r"|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3})\.\d{1,3}\b"
    ),
}

# The values sanitise() puts in. Matched by the patterns above by design -- they are the same
# SHAPE, which is the point of a plausible stand-in -- and subtracted afterwards.
#
# Deliberately not folded into the patterns as negative lookaheads. The first version tried
# that, put the lookahead one group too late, and flagged its own replacement: a clever regex
# that stops matching what it was supposed to is the failure this check exists to prevent.
STAND_INS = {"heliograph-a1b2c3", "SN0000000000", "192.168.1.50"}


def audit(payload: dict) -> list[str]:
    """Anything identifying that survived, as a list of complaints."""
    blob = json.dumps(payload)
    found = []
    for name, pattern in LEAK_PATTERNS.items():
        hits = sorted(set(pattern.findall(blob)) - STAND_INS)
        if hits:
            found.append(f"{name}: {hits[:5]}")
    return found


def capture(host: str) -> dict:
    out = {}
    for name in ENDPOINTS:
        url = f"http://{host}/api/v1/{name}"
        try:
            with urllib.request.urlopen(url, timeout=10) as response:
                out[name] = sanitise(json.load(response))
        except (urllib.error.URLError, json.JSONDecodeError, TimeoutError) as exc:
            # Recorded as absent rather than fatal: a bridge with no capture stored, or discovery
            # never run, legitimately has nothing to say and the page renders that state fine.
            print(f"  {name}: not captured ({exc})")
            out[name] = None
    return out


def extract_page() -> str:
    """The HTML out of the PROGMEM literal, with the C++ escaping undone."""
    source = ASSET.read_text()
    # Any raw-string delimiter, not a hard-coded one: the asset uses R"HTML( today and the
    # delimiter is the author's free choice, so pinning it would break on a rename that changes
    # nothing about the page.
    match = re.search(r'R"([A-Za-z_]*)\((.*)\)\1"', source, re.S)
    if not match:
        raise SystemExit("could not find a raw string literal in " + str(ASSET))
    return match.group(2)


class Handler(http.server.SimpleHTTPRequestHandler):
    page = ""
    data: dict = {}

    def log_message(self, *args):  # noqa: D102 - quiet
        pass

    def do_GET(self):  # noqa: N802 - http.server's spelling
        path = self.path.split("?")[0]
        if path.startswith("/api/v1/"):
            name = path[len("/api/v1/") :].strip("/")
            payload = self.data.get(name)
            if payload is None:
                self.send_error(404)
                return
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/api/v1/events":
            # The page opens an EventSource. Answering 404 lets it fail fast and render from the
            # REST payloads; leaving the request hanging would keep the browser busy past the
            # virtual-time budget and the screenshot would catch a half-drawn page.
            self.send_error(404)
            return

        tab = ""
        if "?" in self.path:
            tab = dict(
                p.split("=", 1)
                for p in self.path.split("?", 1)[1].split("&")
                if "=" in p
            ).get("tab", "")
        body = self.page
        if tab:
            # Clicking rather than setting a variable: the tab button is what loads that tab's
            # data, and a screenshot of a tab whose fetch never ran is a screenshot of nothing.
            body += (
                "<script>addEventListener('load',()=>{"
                f"const b=document.querySelector('[data-t=\"{tab}\"]');b&&b.click();"
                "});</script>"
            )
        encoded = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


def find_chrome() -> str:
    for name in (
        "google-chrome",
        "google-chrome-stable",
        "chromium",
        "chromium-browser",
    ):
        found = shutil.which(name)
        if found:
            return found
    mac = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if pathlib.Path(mac).exists():
        return mac
    raise SystemExit("no Chrome or Chromium on PATH to render with")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bridge", help="host to capture fresh data from, e.g. 192.168.20.254"
    )
    parser.add_argument("--out", default="docs/images", help="where the PNGs go")
    parser.add_argument("--width", type=int, default=1100)
    parser.add_argument("--height", type=int, default=900)
    args = parser.parse_args()

    out_dir = ROOT / args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.bridge:
        print(f"capturing from {args.bridge}")
        data = capture(args.bridge)
        # Refused BEFORE writing. A leak caught after the file is on disk is a leak that may
        # already be in a commit.
        leaks = audit(data)
        if leaks:
            print("REFUSED: identifying data survived sanitisation", file=sys.stderr)
            for leak in leaks:
                print(f"  {leak}", file=sys.stderr)
            print(
                "  add a rule to _field() for the field that carries it",
                file=sys.stderr,
            )
            return 1
        FIXTURE.write_text(json.dumps(data, indent=2) + "\n")
        print(f"  sanitised fixture written to {FIXTURE.relative_to(ROOT)}")
    else:
        if not FIXTURE.exists():
            raise SystemExit(f"no fixture at {FIXTURE}; run once with --bridge <host>")
        data = json.loads(FIXTURE.read_text())

    Handler.page = extract_page()
    Handler.data = data
    chrome = find_chrome()

    with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        for tab, filename in TABS.items():
            target = out_dir / filename
            subprocess.run(
                [
                    chrome,
                    "--headless",
                    "--disable-gpu",
                    "--no-sandbox",
                    "--hide-scrollbars",
                    f"--window-size={args.width},{args.height}",
                    "--virtual-time-budget=4000",
                    f"--screenshot={target}",
                    f"http://127.0.0.1:{port}/?tab={tab}",
                ],
                capture_output=True,
                timeout=120,
            )
            size = target.stat().st_size if target.exists() else 0
            print(f"  {filename:18s} {size // 1024:4d} KB")
            if size == 0:
                print("    (empty -- the page did not render)")
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
