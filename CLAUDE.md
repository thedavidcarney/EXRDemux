# CLAUDE.md

Briefing for Claude Code (or any AI assistant) when working on this repo.
Human-facing context lives in `README.md`.

## What this is

An After Effects effect plugin (`tdcarney EXRDemux`) that demuxes
multilayer EXRs — same job as fnordware's EXtractoR, but with
name-based (not index-based) layer selection so it survives Blender
re-renders. Built for AE 2025; targets Win x64 + macOS arm64.

## Build

```sh
# Win  (vcvars2026 / VC v18 environment required)
cmake --build --preset win-x64-release

# Mac  (run on the Mac mini via SSH — see memory)
cmake --build --preset mac-arm64-release
```

The Mac preset uses the overlay triplet at
`cmake/overlay-triplets/arm64-osx.cmake`, which pins
`VCPKG_OSX_DEPLOYMENT_TARGET=11.0`. Don't strip that — without it,
vcpkg builds OpenEXR/Imath against the build host's SDK and the
binaries fail at runtime on older macOS.

## Release checklist

When cutting a new release (e.g. `v0.2.3`), update **all** of these
together in a single commit before tagging:

1. **`src/exrdemux.cpp`** — five version macros at the top of the file:
   - `EXRDEMUX_MAJOR_VERSION`, `EXRDEMUX_MINOR_VERSION`,
     `EXRDEMUX_BUG_VERSION`, `EXRDEMUX_STAGE_VERSION`,
     `EXRDEMUX_BUILD_VERSION`. These feed `PF_VERSION(...)` in
     `GlobalSetup` and AE checks them at load.
2. **`src/exrdemux.cpp`** — `EXRDEMUX_VERSION_STRING`. This is the
   user-visible string in the About param at the bottom of the Effect
   Controls panel. **This is the one users will read to verify they
   have the right version**, so it MUST match the release tag.
3. **`src/exrdemux.r`** — the encoded `AE_Effect_Version` value (e.g.
   `0x8001`) and the comment above it explaining the encoding. Must
   match the runtime `PF_VERSION(...)` call byte-for-byte or AE refuses
   to load the plugin with a version-mismatch error.
4. **README.md** — if test status changed, update the "Things I didn't
   test very much" list.
5. **Build artifacts** — rebuild Win Release `.aex` + Mac Release
   `.plugin`, zip both with `THIRD_PARTY_LICENSES.txt`, stage in
   `release/EXRDemux-vX.Y.Z-{win-x64,mac-arm64}.zip`.
6. **Git tag** — `vX.Y.Z` matching `EXRDEMUX_VERSION_STRING`.
7. **GitHub Release** — created via the web UI (we don't have `gh`
   wired up yet); attach both zips, paste release notes from the
   commits since the last tag.

After publishing the release, immediately bump `EXRDEMUX_VERSION_STRING`
to the next planned version (e.g. `v0.2.4`) so the in-development
binary doesn't masquerade as the released one.

## Things to be careful about

- **Don't reorder the `PARAM_*` enum.** AE projects persist effect
  parameter values by their integer index, so re-numbering breaks every
  saved project that uses this plugin. New params go at the end with a
  fresh `ID_*` value.
- **AE matchName is capped at 31 chars.** Currently `tdcarney EXRDemux`
  (17 chars). Changing it breaks every loaded project.
- **PiPL out_flags / out_flags2 must match `GlobalSetup`'s runtime
  values byte-for-byte.** Currently `0x06000400` / `0x08001400`. A
  mismatch shows up as an AE error dialog on plugin load.
- **The repo is public.** No SSH endpoints, no usernames, no internal
  file paths in commits, comments, or release notes. (Dev creds /
  endpoints live in Claude's local memory, not in the repo.)
- **No notarization yet.** Mac builds are ad-hoc signed; users need
  `xattr -dr com.apple.quarantine ...` after download. README has the
  command. When notarization is set up, replace this section.

## Where to look

- `src/exrdemux.cpp` — main effect: params, render dispatch, channel
  suite logic, file-direct render, hash-based layer resolution.
- `src/exrdemux_dialog_{win.cpp,mac.mm}` — platform-specific picker
  dialog. Same `ShowLayerPickerDialog(...)` interface.
- `src/exrdemux.r` — PiPL resource (Apple Resource format). Compiled
  by Rez on Mac (with `-useDF`) or by cl/PiPLtool on Win.
- `cmake/overlay-triplets/arm64-osx.cmake` — vcpkg deployment-target
  pin for the Mac build. Don't remove.
- `tests/` — was deleted during cleanup; if you need to re-add
  diagnostics, prefer ad-hoc Python via `OpenEXR` pip package or
  drop one-off scripts in `%TEMP%`.
