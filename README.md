# EXRDemux

Scriptable multilayer EXR plugin for Adobe After Effects 2025.
Replacement for fnordware's EXtractoR with name-based channel
persistence (selections survive EXR re-renders that shift channel
ordering) and full ExtendScript scriptability.

## Status

- Windows build working end to end. Pick a layer, get its pixels.
- Render path uses `PF_ChannelSuite` — comparable speed to EXtractoR.
- macOS scaffolding present but unverified on real hardware. See
  `docs/mac_setup.md` for one-time Mac mini setup.

## Build (Windows)

Prereqs: Build Tools for Visual Studio 2022 or 2026 (Desktop dev with C++),
CMake 3.25+, Git. AE SDK 25.6 dropped under
`third_party/AfterEffectsSDK/Win/` (gitignored — see Adobe Developer
portal). vcpkg is auto-cloned at `third_party/vcpkg/` per the preset.

```sh
cmake --preset win-x64-debug
cmake --build --preset win-x64-debug
# .aex lands in build/win-x64-debug/EXRDemux.aex
```

Install: copy the `.aex` to
`C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\EXRDemux\`,
restart AE, look for "EXRDemux" under Effect & Presets / EXR.

## Build (macOS)

Not tried yet — see [`docs/mac_setup.md`](docs/mac_setup.md). Code
written blind on Windows; first Mac build is expected to need a few
small fixes.

## Key files

| Path | What |
|------|------|
| `src/exrdemux.cpp` | Main plugin: params, render, channel suite |
| `src/exrdemux_dialog_*.{cpp,mm}` | Layer picker modal, per-platform |
| `src/exrdemux.r` | PiPL (Apple Resource format, shared Win+Mac) |
| `tests/jsx/` | Feasibility tests + scripting smoke tests + helpers |
| `tests/jsx/exrdemux_helpers.jsx` | `exrdemuxSetLayer(effect, layerName)` for automation |
| `docs/mac_setup.md` | Mac mini one-time setup |
| `future-ae-plugin-brief.md` | Original starter brief (historical) |

## Background

Built to replace EXtractoR's manual per-layer dropdown clicking with
JSX-driven automation. The original ExtraCtor stores channel selection
as integer indices, which break when a Blender re-render shifts the
channel set. EXRDemux stores a hash of the layer name and resolves it
against the EXR's current channels at render time.

License: MIT.
