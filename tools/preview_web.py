#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Serves the web UI on localhost with tools/demo_fleet.js attached, so the page can be clicked
# through with no bridge, no inverter and no network.
#
# What it serves is the page as the DEVICE serves it -- pulled out of the raw string literal and
# put through tools/build_web.py's stripper. Previewing the authored source would be previewing
# something no browser ever receives.
#
#     python3 tools/preview_web.py           # http://127.0.0.1:8000
#     python3 tools/preview_web.py --port 9000
#
# The fleet is fixed: one legacy single-phase PV inverter, one three-phase hybrid with a
# battery, and one configured device that never replied. Nothing is written anywhere, no request
# leaves the machine, and every PATCH is answered with a cheerful lie -- this is for looking at
# the page, not for testing the firmware.

import argparse
import http.server
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import build_web  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def page() -> bytes:
    """The served page with the demo fleet spliced in ahead of its own script."""
    stub = (ROOT / "tools" / "demo_fleet.js").read_text(encoding="utf-8")
    return build_web.inject_before_script(build_web.served_page(), stub).encode("utf-8")


class Handler(http.server.BaseHTTPRequestHandler):
    body = b""

    def do_GET(self):  # noqa: N802  (http.server's own casing)
        # Every path, not only "/": the page is a single document and a stray request for a
        # favicon or a stylesheet should get the page rather than a 404 in the console.
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args):
        pass  # one line per request drowns out the one line that matters


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    Handler.body = page()
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
        print(
            f"preview_web: http://127.0.0.1:{args.port}  ({len(Handler.body)} bytes, Ctrl-C to stop)"
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
