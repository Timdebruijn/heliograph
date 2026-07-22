<!-- Thanks for contributing! A short description of what and why is enough. -->

## What does this change?

## Checklist

- [ ] `pio test -e native` passes
- [ ] `bash tools/check_layering.sh` passes (brand knowledge only in `src/drivers/`)
- [ ] For a new device profile: `python3 tools/gen_profiles.py --check` passes and the
      sources for every register are listed in the TOML header comment
- [ ] For web UI changes: `python3 tools/check_web_js.py` passes
- [ ] Tested on real hardware? Say which device — "no hardware" is fine too, it just
      stays Experimental until someone confirms it
