# EXRDemux

Scriptable multilayer EXR plugin for Adobe After Effects 2025.
Workflow-compatible reimplementation of fnordware's EXtractoR with
name-based layer persistence (selections survive EXR re-renders that
shift channel ordering) and ExtendScript-accessible parameters.

> ## ⚠️ Early prototype — don't put this in your production pipeline
>
> This is a v0.x experimental release. The core re-render-persistence
> behaviour has been verified against several scenarios (count change,
> name swap, alphabetical insertion, slot swap), but broader testing
> is limited: only AE 2025 on Windows has been exercised, the Mac
> build is unverified, Black Point / White Point / UnMult haven't been
> stress-tested, and behaviour alongside other effects is essentially
> untried. Use it for evaluation and feedback; keep EXtractoR as your
> production tool for now.
>
> ## How this was built
>
> EXRDemux was vibe-coded with [Claude](https://www.anthropic.com/claude)
> (Anthropic). The C++ was written conversationally by Claude based on
> direction, testing, and architectural calls from David Carney. David
> doesn't claim credit for the implementation work — that's Claude's.
> What David brought was the problem statement, the production-workflow
> knowledge, the test cases, and the judgment about what to ship.

## What it does differently from EXtractoR

- **Layer selection persists by name**, not by channel index. Re-render
  the EXR in Blender — add a pass, remove one, swap orderings — and
  every EXRDemux instance keeps pointing at the layer you originally
  picked. EXtractoR uses indices and silently shifts to the wrong
  channel when the file changes.
- **The selection is scriptable.** A standard `effect.property(...)`
  read/write from ExtendScript can set or query the layer, so you can
  build importer scripts on top.
- **The "Pick Layer..." dialog reads the EXR file directly** every
  time, so the layer list is always current and shows names in the
  authoring order Blender wrote them — not alphabetical.
- **Missing layers fail loudly.** Delete a pass and the instance that
  pointed at it goes transparent with `Layer: (missing)` in the label,
  rather than silently rendering whatever pixels now sit at that slot.
- **Black Point / White Point / UnMult** behave the same as EXtractoR's
  equivalents — drop-in replacements for those controls.

## Install (Windows)

1. Quit After Effects.
2. Create the folder `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\EXRDemux\` (admin rights required).
3. Drop `EXRDemux.aex` into it.
4. Launch AE. The effect appears under **Effect → EXR → EXRDemux**.

AE 2025 only — the install path bakes in the version. For other AE
versions, use the per-version Plug-ins folder under that AE install.

## Build (Windows)

Prereqs: Build Tools for Visual Studio 2022 or 2026 (Desktop dev with
C++), CMake 3.25+, Git. AE SDK 25.6 dropped under
`third_party/AfterEffectsSDK/Win/` (gitignored — see Adobe Developer
portal). vcpkg is auto-cloned at `third_party/vcpkg/` per the preset.

```sh
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug
# .aex lands in build/win-x64-debug/EXRDemux.aex
```

To enable the diagnostic perf log (writes to `%USERPROFILE%\Desktop\exrdemux_perf.txt`),
set `EXRDEMUX_PERF_LOG=1` in your environment before launching AE.
Off by default.

## Build (macOS)

Not tried yet — see [`docs/mac_setup.md`](docs/mac_setup.md). Code was
written blind on Windows; first Mac build is expected to need a few
small fixes.

## Key files

| Path | What |
|------|------|
| `src/exrdemux.cpp` | Main plugin: params, render, channel suite, file fallback |
| `src/exrdemux_dialog_*.{cpp,mm}` | Layer picker modal, per-platform |
| `src/exrdemux.r` | PiPL (Apple Resource format, shared Win+Mac) |
| `tests/jsx/` | Feasibility tests + scripting smoke tests + helpers |
| `tests/jsx/exrdemux_helpers.jsx` | `exrdemuxSetLayer(effect, layerName)` for automation |
| `docs/mac_setup.md` | Mac mini one-time setup |

## Acknowledgements

EXRDemux exists because Brendan Bolles / fnordware built
[EXtractoR](https://www.fnordware.com/ProEXR/) and it has been the
standard tool for multilayer EXRs in AE for nearly twenty years.
None of his code is in this binary, but EXRDemux's parameter design
follows EXtractoR's conventions deliberately, and his long-running
contributions to OpenEXR itself underpin the libraries we link.
Thanks, Brendan.

EXRDemux statically links **OpenEXR** and **Imath** (both BSD-3-Clause).
Full attribution and license text is in
[`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt) — that file
ships next to the `.aex` in each release as required by their licenses.

## License

EXRDemux's own code is MIT-licensed — see [`LICENSE`](LICENSE).
TL;DR: do whatever you want, don't sue. The bundled OpenEXR / Imath
binaries retain their own (BSD-3-Clause) terms; see
[`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt).
