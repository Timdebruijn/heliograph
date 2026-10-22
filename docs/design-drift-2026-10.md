# Design project vs. shipped page — drift report, 2026-10-22

**Verdict: the Claude Design copy is stale, not forked. The repo is the source of truth.
Do not splice `firmware/index.html` into `src/web/assets/index_html.h`.**

Read-only investigation. Nothing under `src/` was touched and no firmware behaviour changed.

## What was compared

| | lines | bytes |
|---|---:|---:|
| Design project `firmware/index.html` (projectId `88636dcf…`, read with `DesignSync get_file`, untruncated) | 1 919 | 129 083 |
| Shipped page: the `R"HTML(…)HTML"` literal of `src/web/assets/index_html.h` @ `b555c77` (main) | 2 502 | 168 877 |

`diff` reports 899 lines of output. Line-level, that is 72 hunks: **680 lines only in the repo,
97 lines only in the design copy** (the 97 are mostly the older side of replaced lines).
The repo copy is 583 lines longer.

One caveat on the baseline: the one-line CSS fix of 2026-10-22 (`.tiles>.card+.card{margin-top:0}`)
is on branch `fix/grid-card-stacking-margin` (`793df01`), not yet on `main`. The design copy
has it, `main` does not. That is not drift, it is a pending merge, and it is hunk #1 below.

## Classification

The task's three classes, with the direction of each stated honestly. Hunks are difflib
opcodes over whole lines; "replace" hunks were each read, not just counted.

| Class | Hunks | What it is |
|---|---:|---|
| **(a) in the repo, missing from the design copy** | **21 pure inserts + 47 replaces where the repo side is the newer version of the same code = 68** | Everything the repo gained after the design copy was taken. Every one of the 68 traces to a named commit (table below). |
| **(b) in the design copy, missing from the repo** | **4** | The grid-margin CSS fix (#1, merge pending) and the `spark()` `fill`/`pad` option (#6, #7, #8). Nothing else. |
| **(c) genuinely divergent, same thing worded or built differently** | **0 that survive reading** | Every replace hunk turned out to be repo-newer, not a sideways difference. The one candidate is the `spark()` option, discussed below. |

So **68 of 72 hunks run repo → design (design is behind)**, 3 run design → repo, and 1 is an
unmerged fix present in both places.

The mechanical split, for reference: 21 pure inserts (340 lines, all repo-only), 2 pure deletes
(6 lines, all design-only), 49 replaces (340 repo lines vs 91 design lines).

## Class (a): what a splice would regress

Spot-checked with `git log --reverse -S'<token>' -- src/web/assets/index_html.h`. All first
appearances are repo commits; none came from the Design project.

| Block missing from the design copy | Hunks | First appears in |
|---|---|---|
| Inline `<link rel="icon">` + the explanatory comment about shipping it inside the page | #0 | `2cb28ef` 2026-09-03 (#203) |
| `safeUrl()` and the delegated `data-act` click listener; every `onclick="…${esc(id)}…"` rewritten to `data-act`/`data-val` (stored-XSS via a device serial reported over RS485) | #2, #3, #27, #36, #37, #42, #62, #65 | `57ba288` 2026-08-29 (#196) |
| `isFleet()` — one definition of "is this a fleet" instead of two that disagreed | #12, #14, #16, #69 | `78adc6f` 2026-07-28 (#123) |
| Re-key fix: the password half was UTF-8 encoded twice (`ë` → 401 after a rename) | #49, #50 | `78adc6f` |
| Address-collision check keyed on address alone, not driver+address | #56 | `78adc6f`, extended in `86269d5` (#137) |
| `socColour()` / `SOC_LOW` / `SOC_HALF`; charge bar follows level, arrow follows direction | #9, #18–#21 | `6bdca85` 2026-07-29 (#130) |
| `readerIsBusy()` — a refresh no longer shuts an open `<select>`; `paint()` split into `paintTick()` | #22, #66, #67, #70, #71 | `c9731c8` 2026-07-29 (#131) |
| `neverConfigured()` / `firstRunCard()` — first-run invitation instead of a wiring accusation | #13, #15, #23, #24, #25, #26, #30 | `721636d` 2026-07-29 (#132) |
| `addressOf()` / `addressOptionOf()`; `addExtra` picks a free address and a real driver; pending rows editable and removable (`storedRow`, `slotOf`, `removeExtraAt`, `saveDevice(slot)`) | #28, #29, #32–#39, #51–#54, #57–#61 | `1369106` (#134), `7a32453` (#136), `568f009` (#142), all 2026-07-29 |
| `liveFields()`, `drawIfChanged()`, `countersHtml()` — live readouts updated in place, tabs redrawn only when their signature changes | #41, #43, #45–#48, #68 | `7a32453` (#136) |
| `canShare()` / "one inverter per bridge" / `gateForms()` | #31, #35, #54, #55 | `568f009` (#142) |
| Extra MPPT 3–5 rows, `grid.power`, `load.power`, `allowed_labels` option labels | #10, #11, #38 | `14700a6` 2026-07-30 (#157) |
| Driver capture card (`driverCaptureHtml`, `startDriverCapture`, `renderDriverCapture`) | #44, #63 | `e8cdf1f` 2026-08-02 (#167) |
| Restore-message wording ("press Undo the last restore in the Backup card") | #64 | `9a8e9b0` 2026-08-03 |
| Comment-only differences (spark doc, bus-address comment, etc.) | #4, #5, #17, #40, #49 | same commits as their code |

Practical consequences of splicing the design copy as it is:

- reintroduces the inline-handler XSS shape that #196 removed (the design copy still has
  `onclick="togglePanel('m:${esc(id)}')"` and friends);
- `tools/check_web_js.py` would fail: it fails when `isFleet()` is missing from
  `index_html.h` (its `FAIL: isFleet() not found` branch). Read from the source, not run
  against the design copy;
- 404 for `/favicon.ico` on every page load comes back;
- a rename of the admin user with a non-ASCII password locks the owner out again;
- a freshly provisioned bridge accuses the owner of a wiring fault again;
- an open dropdown is shut by the next refresh, and a just-added inverter row cannot be
  edited or removed without a reboot;
- MPPT 3–5, net flow and house load vanish from the readings list.

## Class (b): design-only content

1. **Grid-margin CSS fix** (#1) — merge pending on `fix/grid-card-stacking-margin`. Resolves itself.
2. **`spark()` `fill` / `pad` options** (#6, #7, #8). `opts.fill` appears nowhere in the repo's
   history (`git log --all -S'opts.fill'` is empty). The repo's own comment on `spark()` says
   there is deliberately no such option ("keeping a `fill` option next to that paragraph would
   leave the footgun loaded beside the warning"). None of the three `spark()` call sites passes
   `fill` or `pad` in either copy, so in the design copy it is dead code.
   This is unshipped design work in the literal sense, and it is the only item that could
   be called a genuine disagreement — but see the next section for why it is most likely a
   leftover, not a fork.

## Stale or forked?

**Stale.** Evidence:

- Scanning all 68 revisions of `index_html.h`, the design copy is closest to `78adc6f`
  (#123, 2026-07-28 23:48): 70 differing lines (measured as lines present in only one of the
  two). The next closest is `6bdca85` at 117, then 188, 249, 304 for the following commits.
  A fork would not have one clear nearest ancestor.
- Of those 70 lines, almost all are the review fixes that were applied *in the same commit*
  (`isFleet`, double-encoding, address-only collision key — each commented "review, 2026-07-28"),
  plus the grid fix and the `spark()` option. So the design copy is the pre-review state of
  #123, and the review fixes went straight into the repo.
- 15 commits have touched `index_html.h` since `78adc6f`, from 2026-07-29 to 2026-09-03. None
  were written back to the Design project. That is the whole drift: **one design → repo
  hand-over (#123), then 15 repo-only commits and no return path.**
- The `spark()` option fits the same story: `78adc6f` already contains the comment rejecting it,
  so the review most likely deleted it before merge. That cannot be proven from git (the design
  project keeps no history), so it is the one item worth a human glance.

The 2026-10-22 observation that "the repo is ahead" is therefore exactly right, and the
~899-line figure is just the cost of those 15 commits.

## Recommendation

1. **The repo is the source of truth.** `src/web/assets/index_html.h` is already where every
   change since 2026-07-29 was made, and everything that guards it operates on that file:
   `tools/check_web_js.py`, `tools/check_dashboard_layout.py`, `tools/build_web.py` (a pre-build
   step, `platformio.ini`), and the CI jobs in `ci.yml` / `release.yml`. The Design project has
   none of that.
2. **Stop splicing from the Design project.** Never overwrite `index_html.h` from
   `firmware/index.html`. If Claude Design is used for visual exploration again, treat it as a
   sketchpad and port the wanted change by hand, hunk-wise, into the repo.
3. **No wholesale write-back to the Design project.** It closes on 2026-12-14, `DesignSync` is
   documented as incremental, and the copy there has no consumer. If a current reference is
   wanted for visual iteration, upload one fresh extract of the shipped literal once, labelled
   as a snapshot. That is optional.
4. **Before 2026-12-14**, decide whether anything else in the project is worth keeping. This
   report did not read `ref/dashboard.png`, `Heliograph *.dc.html`, `PR.md`, `ESP-weight.md`,
   `github.md`, `firmware/demo-stub.js` or `firmware/index-demo.html`. The design project's
   `tools/build_web.py` is a stale copy of the repo's (its header still quotes v0.14.0
   sizes; the repo's now has the output uncommitted and runs from `platformio.ini`).
   Nothing found points at a repo dependency on any of them: no file in the repo mentions
   the Design project or `DesignSync`.
5. **Merge `fix/grid-card-stacking-margin`** so `main` has the one design-side change that is
   real.
6. **Optional, low value:** decide whether `spark()` should ever grow a `pad` option. `fill`
   should not, per the repo's own comment.

## How to reproduce

```bash
# shipped literal -> file (python3 -I, no repo imports)
python3 -I -c "t=open('src/web/assets/index_html.h',encoding='utf-8').read(); \
open('/tmp/shipped.html','w',encoding='utf-8').write(t[t.index('R\"HTML(')+7:t.rindex(')HTML\"')])"
# design copy: DesignSync get_file {projectId: 88636dcf-8c8f-469a-9971-cfab1bb8b437,
#   path: firmware/index.html}; the result is JSON, write its `content` field to /tmp/design.html
diff /tmp/design.html /tmp/shipped.html | wc -l      # 899
# nearest ancestor: for each rev of the header, take the literal and count lines not in the other set
git log --format=%h -- src/web/assets/index_html.h
```

Method limits: hunks are line-level, so one logical change can span several; attribution is
by first appearance of a distinctive token (`git log -S`), which dates a block, not each line
inside it. The design project exposes no history, so "which side is newer" for the `spark()`
option is inferred, not shown.
