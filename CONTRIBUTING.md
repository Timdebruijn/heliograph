# Contributing

Thanks for wanting to help! Two rules of the house first — they explain most review
feedback you might get:

1. **Brand knowledge lives only in `src/drivers/<driver>/`** (and in `profiles/`).
   The canonical model, transports and outputs never name a manufacturer — not even
   in comments. `tools/check_layering.sh` enforces this in CI.
2. **Unknown is never zero.** A value the device did not report is `null`/absent,
   everywhere: REST, MQTT, Modbus, the web UI. No exceptions.

## Adding support for a device

The most valuable contribution, and for Modbus devices usually **no C++ is needed**:
a TOML file in `profiles/` describes the register map, and the build generates the
rest. The whole process — how to research a register map, how to write and validate
the file, how support levels work — is in
[docs/adding-a-device.md](docs/adding-a-device.md).

Access to real hardware matters more than anything else. A profile transcribed from
a PDF starts as Experimental; it is promoted only after someone confirms the values
against a real device's display.

## Before you open a PR

```bash
pio test -e native                    # host test suite
bash tools/check_layering.sh          # architecture invariants
python3 tools/gen_profiles.py --check # profile schema (when touching profiles/)
python3 tools/check_web_js.py         # embedded JS (when touching src/web/)
```

CI runs the same checks, plus both firmware builds.

## Code style

Match the file you are editing. Highlights: C++17, no heap in hot paths, no new
dependencies without a concrete reason, comments explain *why* (constraints,
protocol quirks) rather than *what*. Commit messages and code are in English.

## Licensing

By contributing you agree that your contribution is licensed under the MIT license
of this repository. Protocol knowledge derived from other open-source projects must
be credited in [LICENSE-THIRD-PARTY.md](LICENSE-THIRD-PARTY.md) — reimplement,
don't copy code.
