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
5. **Build + package** — rebuild Win Release `.aex` and Mac Release
   `.plugin`, then run `python scripts/package_iftk.py` on each machine.
   It writes `release/iftk/EXRDemux-X.Y.Z-{win,mac}.zip` in IFTK's
   layout (`plugins/` + `support/THIRD_PARTY_LICENSES.txt`, no scripts).
   Copy the Mac zip back to Windows.
6. **Test the Mac build in AE** — automatically, no hand testing (local
   harness in `tests/iftk_neutral/mac_test/`, gitignored).
7. **Git tag** — `vX.Y.Z` matching `EXRDEMUX_VERSION_STRING`.
8. **GitHub Release** — `gh release create vX.Y.Z` with both zips and
   notes from the commits since the last tag.
9. **IFTK** — from the `noschmo-catalog` checkout:
   `python scripts/publish.py release exrdemux --version X.Y.Z --mac <mac zip> --win <win zip> --notes "..."`.
   It uploads to the catalog's own release and pushes `catalog.json`
   (David OK'd this for his own plugins). Check the two sha256 values
   in `tools/exrdemux.json` match the local zips. Never change the
   catalog id `exrdemux`. To update the guide or preview, edit
   `notes/iftk/` here, then copy into the catalog's `guides/` /
   `publish.py set exrdemux --thumb`.

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
- **No notarization yet.** Mac builds are ad-hoc signed. IFTK clears
  `com.apple.quarantine` itself; only hand installs need the `xattr`
  command in the README. The bundle must pass
  `codesign -v --strict --deep` (the post-build re-sign takes care of
  it). When notarization is set up, replace this section.
- **One copy only.** IFTK installs to `MediaCore\NoSchmo` (Win) and
  `<AE>/Plug-ins/NoSchmo` (Mac). Dev builds go there via
  `--target deploy`, never into another Plug-ins folder.

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
